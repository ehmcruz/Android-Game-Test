/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>

#include <string>
#include <iostream>

#include "native_engine.hpp"

// verbose debug logs on?
#define VERBOSE_LOGGING 1

#if VERBOSE_LOGGING
#define VLOGD LOGD
#else
#define VLOGD
#endif

// max # of GL errors to print before giving up
#define MAX_GL_ERRORS 200

static NativeEngine *_singleton = NULL;

NativeEngine::NativeEngine(struct android_app *app) {
    LOGD("NativeEngine: initializing.");
    mApp = app;
    mHasFocus = mIsVisible = mHasWindow = false;
    mHasGLObjects = false;
    mEglDisplay = EGL_NO_DISPLAY;
    mEglSurface = EGL_NO_SURFACE;
    mEglContext = EGL_NO_CONTEXT;
    mEglConfig = 0;
    mSurfWidth = mSurfHeight = 0;
    mApiVersion = 0;
    mJniEnv = NULL;
    memset(&mState, 0, sizeof(mState));
    mIsFirstFrame = true;
    ogl_loaded = false;
    vs_loaded = false;
    fs_loaded = false;
    nn = 0;

    const float orig_x_[3] = { 0.0f,  -0.5f, 0.5f };
    const float orig_y_[3] = { -0.5f, 0.5f, 0.5f };

    memcpy(this->orig_x, orig_x_, sizeof(orig_x_));
    memcpy(this->orig_y, orig_y_, sizeof(orig_y_));

    if (app->savedState != NULL) {
        // we are starting with previously saved state -- restore it
        mState = *(struct NativeEngineSavedState*) app->savedState;
    }

    // only one instance of NativeEngine may exist!
    MY_ASSERT(_singleton == NULL);
    _singleton = this;

    VLOGD("NativeEngine: querying API level.");
    LOGD("NativeEngine: API version %d.", mApiVersion);
}

NativeEngine* NativeEngine::GetInstance() {
    MY_ASSERT(_singleton != NULL);
    return _singleton;
}

NativeEngine::~NativeEngine() {
    VLOGD("NativeEngine: destructor running");
    KillContext();
    if (mJniEnv) {
        LOGD("Detaching current thread from JNI.");
        mApp->activity->vm->DetachCurrentThread();
        LOGD("Current thread detached from JNI.");
        mJniEnv = NULL;
    }
    _singleton = NULL;
}

static void _handle_cmd_proxy(struct android_app* app, int32_t cmd) {
    NativeEngine *engine = (NativeEngine*) app->userData;
    engine->HandleCommand(cmd);
}

bool NativeEngine::IsAnimating() {
    return mHasFocus && mIsVisible && mHasWindow;
}

void NativeEngine::callback_touch_screen_event (const TouchScreenEvent& event)
{
    const char *dir;
    std::string complement;

    switch (event.type) {
        using enum TouchScreenEvent::Type;

        case Up:
            dir = "Up";

            break;

        case Down:
            dir = "Down";
            break;

        case Move:
            dir = "Move";
            complement = "Moving " + std::to_string(event.move_ndelta.x) + ", " + std::to_string(event.move_ndelta.y);

            for (int i = 0; i < 3; i++) {
                orig_x[i] += event.move_ndelta.x;
                orig_y[i] += event.move_ndelta.y;
            }

            break;
    }

    LOGD("%s event x=%.4f y=%.4f pointer_count=%u pointer_index=%u id=%i %s\n", dir, event.norm_pos.x, event.norm_pos.y, event.motion_event->pointerCount, event.pointer_index, event.id, complement.c_str());
}

void NativeEngine::process_input_events ()
{
    android_input_buffer* inputBuffer = android_app_swap_input_buffers(mApp);

    if (inputBuffer && inputBuffer->motionEventsCount) {
        for (uint32_t i = 0; i < inputBuffer->motionEventsCount; ++i) {
            GameActivityMotionEvent* motionEvent = &inputBuffer->motionEvents[i];

            if (motionEvent->pointerCount > 0) {
                const int action = motionEvent->action;
                const int actionMasked = action & AMOTION_EVENT_ACTION_MASK;
                // Initialize pointerIndex to the max size, we only cook an
                // event at the end of the function if pointerIndex is set to a valid index range
                uint32_t pointerIndex = GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT;

                TouchScreenEvent ev;

                ev.motion_event = motionEvent;

                // use screen size as the motion range
                ev.min.x = 0.0f;
                ev.min.y = 0.0f;

                ev.max.x = static_cast<float>(mSurfWidth);
                ev.max.y = static_cast<float>(mSurfHeight);

                switch (actionMasked) {
                    case AMOTION_EVENT_ACTION_DOWN:
                        pointerIndex = 0;
                        ev.type = TouchScreenEvent::Type::Down;
                        break;
                    case AMOTION_EVENT_ACTION_POINTER_DOWN:
                        pointerIndex = ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
                        ev.type = TouchScreenEvent::Type::Down;
                        break;
                    case AMOTION_EVENT_ACTION_UP:
                        pointerIndex = 0;
                        ev.type = TouchScreenEvent::Type::Up;
                        break;
                    case AMOTION_EVENT_ACTION_POINTER_UP:
                        pointerIndex = ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
                        ev.type = TouchScreenEvent::Type::Up;
                        break;
                    case AMOTION_EVENT_ACTION_MOVE: {
                        // Move includes all active pointers, so loop and process them here,
                        // we do not set pointerIndex since we are cooking the events in
                        // this loop rather than at the bottom of the function
                        ev.type = TouchScreenEvent::Type::Move;

                        for (uint32_t i = 0; i < motionEvent->pointerCount; ++i) {
                            //_cookEventForPointerIndex(motionEvent, callback, ev, i);
                            ev.pointer_index = i;
                            ev.id = motionEvent->pointers[i].id;
                            ev.pos.x = GameActivityPointerAxes_getX(&motionEvent->pointers[i]);
                            ev.pos.y = GameActivityPointerAxes_getY(&motionEvent->pointers[i]);
                            ev.norm_pos.x = ev.pos.x / ev.max.x;
                            ev.norm_pos.y = ev.pos.y / ev.max.y;

                            // calculate motion delta

                            auto it = std::find_if(
                                    this->previous_positions.begin(),
                                    this->previous_positions.end(),
                                    [&ev] (const std::pair<int32_t, Position>& el) -> bool {
                                        return el.first == ev.id;
                                    }
                            );

                            if (it == this->previous_positions.end())
                                LOGE("baaaaaaaaaaaaaaaaad\n");
                            else {
                                Position& pp = it->second;

                                ev.move_ndelta.x = ev.norm_pos.x - pp.x;
                                ev.move_ndelta.y = ev.norm_pos.y - pp.y;

                                // update previous position
                                pp.x = ev.norm_pos.x;
                                pp.y = ev.norm_pos.y;
                            }

                            this->callback_touch_screen_event(ev);
                        }
                        break;
                    }
                    default:
                        break;
                }

                // Only cook an event if we set the pointerIndex to a valid range, note that
                // move events cook above in the switch statement.
                if (pointerIndex != GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT) {
                    ev.pointer_index = pointerIndex;
                    //_cookEventForPointerIndex(motionEvent, callback, ev, pointerIndex);
                    ev.id = motionEvent->pointers[pointerIndex].id;
                    ev.pos.x = GameActivityPointerAxes_getX(&motionEvent->pointers[pointerIndex]);
                    ev.pos.y = GameActivityPointerAxes_getY(&motionEvent->pointers[pointerIndex]);
                    ev.norm_pos.x = ev.pos.x / ev.max.x;
                    ev.norm_pos.y = ev.pos.y / ev.max.y;

                    switch (ev.type) {
                        using enum TouchScreenEvent::Type;

                        case Up: {
                            bool found = false;

                            this->previous_positions.remove_if(
                                    [&ev, &found](const std::pair<int32_t, Position> &el) -> bool {
                                        if (el.first == ev.id) {
                                            found = true;
                                            return true;
                                        } else
                                            return false;
                                    }
                            );

                            if (!found)
                                LOGE("noooooooooooooo\n");

                            break;
                        }

                        case Down:
                            std::pair<int32_t, Position> pp(ev.id, ev.norm_pos);
                            this->previous_positions.push_back(pp);
                            break;
                    }

                    this->callback_touch_screen_event(ev);
                }
            }
        }

        android_app_clear_motion_events(inputBuffer);
    }
}

void NativeEngine::GameLoop() {
    mApp->userData = this;
    mApp->onAppCmd = _handle_cmd_proxy;

    g_vertex_buffer_data[0] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -0.5f, 0.0f, 0.0f };
    g_vertex_buffer_data[1] = { 0.0f, 1.0f, 0.0f, 0.0f, -0.5f, 0.5f, 0.0f, 0.0f };
    g_vertex_buffer_data[2] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, 0.0f, 0.0f };

    while (1) {
        int ident, events;
        struct android_poll_source* source;

        // If not animating, block until we get an event; if animating, don't block.
        while ((ident = ALooper_pollAll(IsAnimating() ? 0 : -1, NULL, &events,
                (void**) &source)) >= 0) {

            // process event
            if (source != NULL) {
                source->process(mApp, source);
            }

            // are we exiting?
            if (mApp->destroyRequested) {
                return;
            }
        }

        this->process_input_events();

//        if (IsAnimating()) {
            DoFrame();
//        }
    }
}

JNIEnv* NativeEngine::GetJniEnv() {
    if (!mJniEnv) {
        LOGD("Attaching current thread to JNI.");
        if (0 != mApp->activity->vm->AttachCurrentThread(&mJniEnv, NULL)) {
            LOGE("*** FATAL ERROR: Failed to attach thread to JNI.");
            ABORT_GAME;
        }
        MY_ASSERT(mJniEnv != NULL);
        LOGD("Attached current thread to JNI, %p", mJniEnv);
    }

    return mJniEnv;
}


void NativeEngine::HandleCommand(int32_t cmd) {
    //SceneManager *mgr = SceneManager::GetInstance();

    VLOGD("NativeEngine: handling command %d.", cmd);
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.
            VLOGD("NativeEngine: APP_CMD_SAVE_STATE");
            mApp->savedState = malloc(sizeof(mState));
            *((NativeEngineSavedState*) mApp->savedState) = mState;
            mApp->savedStateSize = sizeof(mState);
            break;
        case APP_CMD_INIT_WINDOW:
            // We have a window!
            VLOGD("NativeEngine: APP_CMD_INIT_WINDOW");
            if (mApp->window != NULL) {
                mHasWindow = true;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is going away -- kill the surface
            VLOGD("NativeEngine: APP_CMD_TERM_WINDOW");
            KillSurface();
            mHasWindow = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            VLOGD("NativeEngine: APP_CMD_GAINED_FOCUS");
            mHasFocus = true;
            break;
        case APP_CMD_LOST_FOCUS:
            VLOGD("NativeEngine: APP_CMD_LOST_FOCUS");
            mHasFocus = false;
            break;
        case APP_CMD_PAUSE:
            VLOGD("NativeEngine: APP_CMD_PAUSE");
 //           mgr->OnPause();
            break;
        case APP_CMD_RESUME:
            VLOGD("NativeEngine: APP_CMD_RESUME");
 //           mgr->OnResume();
            break;
        case APP_CMD_STOP:
            VLOGD("NativeEngine: APP_CMD_STOP");
            mIsVisible = false;
            break;
        case APP_CMD_START:
            VLOGD("NativeEngine: APP_CMD_START");
            mIsVisible = true;
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            VLOGD("NativeEngine: %s", cmd == APP_CMD_WINDOW_RESIZED ?
                "APP_CMD_WINDOW_RESIZED" : "APP_CMD_CONFIG_CHANGED");
            // Window was resized or some other configuration changed.
            // Note: we don't handle this event because we check the surface dimensions
            // every frame, so that's how we know it was resized. If you are NOT doing that,
            // then you need to handle this event!
            break;
        case APP_CMD_LOW_MEMORY:
            VLOGD("NativeEngine: APP_CMD_LOW_MEMORY");
            // system told us we have low memory. So if we are not visible, let's
            // cooperate by deallocating all of our graphic resources.
            if (!mHasWindow) {
                VLOGD("NativeEngine: trimming memory footprint (deleting GL objects).");
                KillGLObjects();
            }
            break;
        default:
            VLOGD("NativeEngine: (unknown command).");
            break;
    }

    VLOGD("NativeEngine: STATUS: F%d, V%d, W%d, EGL: D %p, S %p, CTX %p, CFG %p",
        mHasFocus, mIsVisible, mHasWindow, mEglDisplay, mEglSurface, mEglContext,
        mEglConfig);
}


bool NativeEngine::InitDisplay() {
    if (mEglDisplay != EGL_NO_DISPLAY) {
        // nothing to do
        LOGD("NativeEngine: no need to init display (already had one).");
        return true;
    }

    LOGD("NativeEngine: initializing display.");
    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (EGL_FALSE == eglInitialize(mEglDisplay, 0, 0)) {
        LOGE("NativeEngine: failed to init display, error %d", eglGetError());
        return false;
    }
    return true;
}

bool NativeEngine::InitSurface() {
    // need a display
    MY_ASSERT(mEglDisplay != EGL_NO_DISPLAY);

    if (mEglSurface != EGL_NO_SURFACE) {
        // nothing to do
        LOGD("NativeEngine: no need to init surface (already had one).");
        return true;
    }

    LOGD("NativeEngine: initializing surface.");

    EGLint numConfigs;

    const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, // request OpenGL ES 2.0
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 16,
            EGL_NONE
    };

    // since this is a simple sample, we have a trivial selection process. We pick
    // the first EGLConfig that matches:
    eglChooseConfig(mEglDisplay, attribs, &mEglConfig, 1, &numConfigs);

    // create EGL surface
    mEglSurface = eglCreateWindowSurface(mEglDisplay, mEglConfig, mApp->window, NULL);
    if (mEglSurface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL surface, EGL error %d", eglGetError());
        return false;
    }

    LOGD("NativeEngine: successfully initialized surface.");
    return true;
}

bool NativeEngine::InitContext() {
    // need a display
    MY_ASSERT(mEglDisplay != EGL_NO_DISPLAY);

    EGLint attribList[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}; // OpenGL ES 3.0

    if (mEglContext != EGL_NO_CONTEXT) {
        // nothing to do
        LOGD("NativeEngine: no need to init context (already had one).");
        return true;
    }

    LOGD("NativeEngine: initializing context.");

    // create EGL context
    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, NULL, attribList);
    if (mEglContext == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL context, EGL error %d", eglGetError());
        return false;
    }

    LOGD("NativeEngine: successfull initialized context.");

    return true;
}

void NativeEngine::ConfigureOpenGL() {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  //  glEnable(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
  //  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glClear(GL_COLOR_BUFFER_BIT);
}

enum t_attrib_id {
    attrib_position,
    attrib_color,
    attrib_offset
} t_attrib_id;

void NativeEngine::load_vertex_shader ()
{
    static const char* vertex_shader =
        "#version 300 es\n"
        "in vec2 i_position;\n"
        "in vec4 i_color;\n"
        "in vec2 i_offset;\n"
        "out vec4 v_color;\n"
//        "uniform mat4 u_projection_matrix;\n"
        "void main() {\n"
        "    v_color = i_color;\n"
        "    gl_Position = vec4( (i_offset + i_position), 0.0, 1.0 );\n"
        "}\n";

    vs = glCreateShader(GL_VERTEX_SHADER);

    int length = strlen(vertex_shader);
    glShaderSource(vs, 1, (const GLchar**)&vertex_shader, &length);
    glCompileShader(vs);

    GLint status;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logSize = 0;
        glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &logSize);

        char* berror = (char*)malloc(logSize);
        MY_ASSERT(berror != nullptr);

        glGetShaderInfoLog(vs, logSize, nullptr, berror);

        LOGE("vertex shader compilation failed");

        LOGE("%s\n", berror);
        free(berror);
    }
    else {
        LOGD("vertex SHADER compiled ok!");
        vs_loaded = true;
    }
}

void NativeEngine::load_frag_shader ()
{
    static const char* fragment_shader =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec4 v_color;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "    o_color = v_color;\n"
        "}\n";

    fs = glCreateShader(GL_FRAGMENT_SHADER);

    int length = strlen(fragment_shader);
    glShaderSource(fs, 1, (const GLchar**)&fragment_shader, &length);
    glCompileShader(fs);

    GLint status;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logSize = 0;
        glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &logSize);

        char* berror = (char*)malloc(logSize);
        MY_ASSERT(berror != nullptr);

        glGetShaderInfoLog(fs, logSize, nullptr, berror);

        LOGE("fragment shader compilation failed");

        LOGE("%s\n", berror);
        free(berror);
    }
    else {
        LOGD("fragment SHADER compiled ok!");
        fs_loaded = true;
    }
}

void NativeEngine::load_program ()
{
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);

    glBindAttribLocation(program, attrib_position, "i_position");
    glBindAttribLocation(program, attrib_color, "i_color");
    glBindAttribLocation(program, attrib_offset, "i_offset");
    glLinkProgram(program);

    LOGD("opengl program linked\n");

    glUseProgram(program);

    LOGD("opengl program use\n");
}

void NativeEngine::setup_vertex_buffer ()
{
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_color);
    glEnableVertexAttribArray(attrib_offset);

    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vertex_t), (void*)(4 * sizeof(float)));
    glVertexAttribPointer(attrib_color, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex_t), 0);
    glVertexAttribPointer(attrib_offset, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vertex_t), (void*)(6 * sizeof(float)));

    LOGD("opengl vertex attribs ok\n");

    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertex_buffer_data), g_vertex_buffer_data, GL_DYNAMIC_DRAW);
}

bool NativeEngine::PrepareToRender() {
    do {
        // if we're missing a surface, context, or display, create them
        if (mEglDisplay == EGL_NO_DISPLAY || mEglSurface == EGL_NO_SURFACE ||
            mEglContext == EGL_NO_CONTEXT) {

            // create display if needed
            if (!InitDisplay()) {
                LOGE("NativeEngine: failed to create display.");
                return false;
            }

            // create surface if needed
            if (!InitSurface()) {
                LOGE("NativeEngine: failed to create surface.");
                return false;
            }

            // create context if needed
            if (!InitContext()) {
                LOGE("NativeEngine: failed to create context.");
                return false;
            }

            LOGD("NativeEngine: binding surface and context (display %p, surface %p, context %p)",
                 mEglDisplay, mEglSurface, mEglContext);

            // bind them
            if (EGL_FALSE == eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext)) {
                LOGE("NativeEngine: eglMakeCurrent failed, EGL error %d", eglGetError());
                HandleEglError(eglGetError());
            }

            // configure our global OpenGL settings
            ConfigureOpenGL();

            load_vertex_shader();
            load_frag_shader();
            load_program();
            setup_vertex_buffer();
        }

        // now that we're sure we have a context and all, if we don't have the OpenGL 
        // objects ready, create them.
/*        if (!mHasGLObjects) {
            LOGD("NativeEngine: creating OpenGL objects.");
            if (!InitGLObjects()) {
                LOGE("NativeEngine: unable to initialize OpenGL objects.");
                return false;
            }
        } */
    } while (0);

    // ready to render
    return true;
}

void NativeEngine::KillGLObjects() {
    if (mHasGLObjects) {
//        SceneManager *mgr = SceneManager::GetInstance();
//        mgr->KillGraphics();
        mHasGLObjects = false;
    }
}

void NativeEngine::KillSurface() {
    LOGD("NativeEngine: killing surface.");
    eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mEglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEglSurface);
        mEglSurface = EGL_NO_SURFACE;
    }
    LOGD("NativeEngine: Surface killed successfully.");
}

void NativeEngine::KillContext() {
    LOGD("NativeEngine: killing context.");

    // since the context is going away, we have to kill the GL objects
    KillGLObjects();

    eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (mEglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mEglDisplay, mEglContext);
        mEglContext = EGL_NO_CONTEXT;
    }
    LOGD("NativeEngine: Context killed successfully.");
}

void NativeEngine::KillDisplay() {
    // causes context and surface to go away too, if they are there
    LOGD("NativeEngine: killing display.");
    KillContext();
    KillSurface();

    if (mEglDisplay != EGL_NO_DISPLAY) {
        LOGD("NativeEngine: terminating display now.");
        eglTerminate(mEglDisplay);
        mEglDisplay = EGL_NO_DISPLAY;
    }
    LOGD("NativeEngine: display killed successfully.");
}

bool NativeEngine::HandleEglError(EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            // nothing to do
            return true;
        case EGL_CONTEXT_LOST:
            LOGW("NativeEngine: egl error: EGL_CONTEXT_LOST. Recreating context.");
            KillContext();
            return true;
        case EGL_BAD_CONTEXT:
            LOGW("NativeEngine: egl error: EGL_BAD_CONTEXT. Recreating context.");
            KillContext();
            return true;
        case EGL_BAD_DISPLAY:
            LOGW("NativeEngine: egl error: EGL_BAD_DISPLAY. Recreating display.");
            KillDisplay();
            return true;
        case EGL_BAD_SURFACE:
            LOGW("NativeEngine: egl error: EGL_BAD_SURFACE. Recreating display.");
            KillSurface();
            return true;
        default:
            LOGW("NativeEngine: unknown egl error: %d", error);
            return false;
    }
}

static void _log_opengl_error(GLenum err) {
    switch (err) {
        case GL_NO_ERROR:
            LOGE("*** OpenGL error: GL_NO_ERROR");
            break;
        case GL_INVALID_ENUM:
            LOGE("*** OpenGL error: GL_INVALID_ENUM");
            break;
        case GL_INVALID_VALUE:
            LOGE("*** OpenGL error: GL_INVALID_VALUE");
            break;
        case GL_INVALID_OPERATION:
            LOGE("*** OpenGL error: GL_INVALID_OPERATION");
            break;
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            LOGE("*** OpenGL error: GL_INVALID_FRAMEBUFFER_OPERATION");
            break;
        case GL_OUT_OF_MEMORY:
            LOGE("*** OpenGL error: GL_OUT_OF_MEMORY");
            break;
        default:
            LOGE("*** OpenGL error: error %d", err);
            break;
    }
}


void NativeEngine::DoFrame() {
    // prepare to render (create context, surfaces, etc, if needed)
    if (!PrepareToRender()) {
        // not ready
        VLOGD("NativeEngine: preparation to render failed.");
        return;
    }

    //    SceneManager *mgr = SceneManager::GetInstance();

        // how big is the surface? We query every frame because it's cheap, and some
        // strange devices out there change the surface size without calling any callbacks...
    int width, height;
    eglQuerySurface(mEglDisplay, mEglSurface, EGL_WIDTH, &width);
    eglQuerySurface(mEglDisplay, mEglSurface, EGL_HEIGHT, &height);

    if (width != mSurfWidth || height != mSurfHeight) {
        // notify scene manager that the surface has changed size
        LOGD("NativeEngine: surface changed size %dx%d --> %dx%d", mSurfWidth, mSurfHeight,
            width, height);
        mSurfWidth = width;
        mSurfHeight = height;
        //        mgr->SetScreenSize(mSurfWidth, mSurfHeight);
        glViewport(0, 0, mSurfWidth, mSurfHeight);

        return;
    }

    // if this is the first frame, install the welcome scene
    if (mIsFirstFrame) {
        mIsFirstFrame = false;
        //        mgr->RequestNewScene(new WelcomeScene());
    }

    // render!
//    mgr->DoFrame();

 //   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    //glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClear(GL_COLOR_BUFFER_BIT);

    {
        static float rotate_by = 0.0f;

        rotate_by = fmod(rotate_by + 3.1415f / 100.0f, 2.0f*3.1415f);
        float s = sin(rotate_by);
        float c = cos(rotate_by);

        for (int i = 0; i < 3; i++) {
            g_vertex_buffer_data[i].x = orig_x[i] * c - orig_y[i] * s;
            g_vertex_buffer_data[i].y = orig_x[i] * s + orig_y[i] * c;
        }
    }

  //  glBindVertexArray( vao );

    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertex_buffer_data), g_vertex_buffer_data, GL_DYNAMIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    if ((nn % 50) == 0) {
        //LOGD("render frame %u\n", nn);
    }
    nn++;

    //LOGD("vs_loaded=%i   fs_loaded=%i\n", vs_loaded, fs_loaded);

    // swap buffers
    if (EGL_FALSE == eglSwapBuffers(mEglDisplay, mEglSurface)) {
        // failed to swap buffers... 
        LOGW("NativeEngine: eglSwapBuffers failed, EGL error %d", eglGetError());
        HandleEglError(eglGetError());
    }

    // print out GL errors, if any
    GLenum e;
    static int errorsPrinted = 0;
    while ((e = glGetError()) != GL_NO_ERROR) {
        if (errorsPrinted < MAX_GL_ERRORS) {
            _log_opengl_error(e);
            ++errorsPrinted;
            if (errorsPrinted >= MAX_GL_ERRORS) {
                LOGE("*** NativeEngine: TOO MANY OPENGL ERRORS. NO LONGER PRINTING.");
            }
        }
    }
}

android_app* NativeEngine::GetAndroidApp() {
    return mApp;
}

bool NativeEngine::InitGLObjects() {
    if (!mHasGLObjects) {
//        SceneManager *mgr = SceneManager::GetInstance();
//        mgr->StartGraphics();
        _log_opengl_error(glGetError());
        mHasGLObjects = true;
    }
    return true;
}
