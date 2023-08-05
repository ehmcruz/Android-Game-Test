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
#ifndef endlesstunnel_native_engine_hpp
#define endlesstunnel_native_engine_hpp

#include <list>
#include <utility>
#include "common.hpp"

struct NativeEngineSavedState {};

struct gl_vertex_t {
    GLfloat r;
    GLfloat g;
    GLfloat b;
    GLfloat alpha;
    GLfloat x;
    GLfloat y;
    GLfloat offset_x;
    GLfloat offset_y;
};

struct Position {
    float x, y;
};

struct TouchScreenEvent {
    enum class Type {
        Up,
        Down,
        Move
    };

    Type type;
    int32_t id;
    Position min, max;
    Position pos;
    Position norm_pos; // normalized x and y [0-1]
    Position move_ndelta; // normalized delta

    GameActivityMotionEvent *motion_event;
    uint32_t pointer_index;
};

class NativeEngine {
    public:
        // create an engine
        NativeEngine(struct android_app *app);
        ~NativeEngine();

        // runs application until it dies
        void GameLoop();

        // returns the JNI environment
        JNIEnv *GetJniEnv();

        // returns the Android app object
        android_app* GetAndroidApp();

        // returns the (singleton) instance
        static NativeEngine* GetInstance();

        void load_vertex_shader ();

        void load_frag_shader ();

        void load_program ();

        void setup_vertex_buffer ();

    private:
        bool ogl_loaded, vs_loaded, fs_loaded;
        GLuint vs, fs, program;
        GLuint vao, vbo;
        gl_vertex_t g_vertex_buffer_data[3];

        // variables to track Android lifecycle:
        bool mHasFocus, mIsVisible, mHasWindow;

        // are our OpenGL objects (textures, etc) currently loaded?
        bool mHasGLObjects;

        // android API version (0 if not yet queried)
        int mApiVersion;

        unsigned nn;

    float orig_x[3];
    float orig_y[3];

    // EGL stuff
        EGLDisplay mEglDisplay;
        EGLSurface mEglSurface;
        EGLContext mEglContext;
        EGLConfig mEglConfig;

        // known surface size
        int mSurfWidth, mSurfHeight;

        // android_app structure
        struct android_app* mApp;

        // additional saved state
        struct NativeEngineSavedState mState;

        // JNI environment
        JNIEnv *mJniEnv;

        // is this the first frame we're drawing?
        bool mIsFirstFrame;

        // initialize the display
        bool InitDisplay();

        // initialize surface. Requires display to have been initialized first.
        bool InitSurface();

        // initialize context. Requires display to have been initialized first.
        bool InitContext();

        std::list< std::pair<int32_t, Position> > previous_positions;
        void process_input_events ();

        void callback_touch_screen_event (const TouchScreenEvent& event);

        // kill context
        void KillContext();
        void KillSurface();
        void KillDisplay(); // also causes context and surface to get killed

        bool HandleEglError(EGLint error);

        bool InitGLObjects();
        void KillGLObjects();

        void ConfigureOpenGL();

        bool PrepareToRender();

        void DoFrame();

        bool IsAnimating();

    public:
        // these are public for simplicity because we have internal static callbacks
        void HandleCommand(int32_t cmd);
};

#endif
