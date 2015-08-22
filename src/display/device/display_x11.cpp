/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2011 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pangolin/platform.h>
#include <pangolin/gl/glinclude.h>
#include <pangolin/gl/glglut.h>
#include <pangolin/display/display.h>
#include <pangolin/display/display_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

namespace pangolin
{
extern __thread PangolinGl* context;
}

Display *display = 0;
Window win = 0;
GLXContext ctx = 0;
Colormap cmap;
const long EVENT_MASKS = ButtonPressMask|StructureNotifyMask;

#define GLX_CONTEXT_MAJOR_VERSION_ARB       0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB       0x2092
typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

// Helper to check for extension string presence.  Adapted from:
//   http://www.opengl.org/resources/features/OGLextensions/
static bool isExtensionSupported(const char *extList, const char *extension)
{
    const char *start;
    const char *where, *terminator;

    /* Extension names should not have spaces. */
    where = strchr(extension, ' ');
    if (where || *extension == '\0')
        return false;

    /* It takes a bit of care to be fool-proof about parsing the
     OpenGL extensions string. Don't be fooled by sub-strings,
     etc. */
    for (start=extList;;) {
        where = strstr(start, extension);

        if (!where)
            break;

        terminator = where + strlen(extension);

        if ( where == start || *(where - 1) == ' ' )
            if ( *terminator == ' ' || *terminator == '\0' )
                return true;

        start = terminator;
    }

    return false;
}

static bool ctxErrorOccurred = false;
static int ctxErrorHandler( Display *dpy, XErrorEvent *ev )
{
    ctxErrorOccurred = true;
    return 0;
}

int CreateX11Window(int width, int height)
{
    display = XOpenDisplay(NULL);

    if (!display)
    {
        printf("Failed to open X display\n");
        exit(1);
    }

    // Get a matching FB config
    static int visual_attribs[] =
    {
        GLX_X_RENDERABLE    , True,
        GLX_DRAWABLE_TYPE   , GLX_WINDOW_BIT,
        GLX_RENDER_TYPE     , GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE   , GLX_TRUE_COLOR,
        GLX_RED_SIZE        , 8,
        GLX_GREEN_SIZE      , 8,
        GLX_BLUE_SIZE       , 8,
        GLX_ALPHA_SIZE      , 8,
        GLX_DEPTH_SIZE      , 24,
        GLX_STENCIL_SIZE    , 8,
        GLX_DOUBLEBUFFER    , True,
        //      GLX_SAMPLE_BUFFERS  , 1,
        //      GLX_SAMPLES         , 2,
        None
    };

    int glx_major, glx_minor;

    // FBConfigs were added in GLX version 1.3.
    if ( !glXQueryVersion( display, &glx_major, &glx_minor ) ||
         ( ( glx_major == 1 ) && ( glx_minor < 3 ) ) || ( glx_major < 1 ) )
    {
        printf("Invalid GLX version");
        exit(1);
    }

    printf( "Getting matching framebuffer configs\n" );
    int fbcount;
    GLXFBConfig* fbc = glXChooseFBConfig(display, DefaultScreen(display), visual_attribs, &fbcount);
    if (!fbc)
    {
        printf( "Failed to retrieve a framebuffer config\n" );
        exit(1);
    }
    printf( "Found %d matching FB configs.\n", fbcount );

    // Pick the FB config/visual with the most samples per pixel
    printf( "Getting XVisualInfos\n" );
    int best_fbc = -1, worst_fbc = -1, best_num_samp = -1, worst_num_samp = 999;

    int i;
    for (i=0; i<fbcount; ++i)
    {
        XVisualInfo *vi = glXGetVisualFromFBConfig( display, fbc[i] );
        if ( vi )
        {
            int samp_buf, samples;
            glXGetFBConfigAttrib( display, fbc[i], GLX_SAMPLE_BUFFERS, &samp_buf );
            glXGetFBConfigAttrib( display, fbc[i], GLX_SAMPLES       , &samples  );

            printf( "  Matching fbconfig %d, visual ID 0x%2x: SAMPLE_BUFFERS = %d,"
                    " SAMPLES = %d\n",
                    i, vi -> visualid, samp_buf, samples );

            if ( best_fbc < 0 || samp_buf && samples > best_num_samp )
                best_fbc = i, best_num_samp = samples;
            if ( worst_fbc < 0 || !samp_buf || samples < worst_num_samp )
                worst_fbc = i, worst_num_samp = samples;
        }
        XFree( vi );
    }

    GLXFBConfig bestFbc = fbc[ best_fbc ];

    // Be sure to free the FBConfig list allocated by glXChooseFBConfig()
    XFree( fbc );

    // Get a visual
    XVisualInfo *vi = glXGetVisualFromFBConfig( display, bestFbc );
    printf( "Chosen visual ID = 0x%x\n", vi->visualid );

    printf( "Creating colormap\n" );
    XSetWindowAttributes swa;
    swa.colormap = cmap = XCreateColormap( display,
                                           RootWindow( display, vi->screen ),
                                           vi->visual, AllocNone );
    swa.background_pixmap = None ;
    swa.border_pixel      = 0;
    swa.event_mask        = StructureNotifyMask;

    printf( "Creating window\n" );
    win = XCreateWindow( display, RootWindow( display, vi->screen ),
                         0, 0, width, height, 0, vi->depth, InputOutput,
                         vi->visual,
                         CWBorderPixel|CWColormap|CWEventMask, &swa );
    if ( !win )
    {
        printf( "Failed to create window.\n" );
        exit(1);
    }

    // Done with the visual info data
    XFree( vi );

    XStoreName( display, win, "GL 3.0 Window" );

    printf( "Mapping window\n" );
    XMapWindow( display, win );

    /* tell the display server what kind of events we would like to see */
    XSelectInput(display, win, EVENT_MASKS );

    // Get the default screen's GLX extension list
    const char *glxExts = glXQueryExtensionsString( display,
                                                    DefaultScreen( display ) );

    // NOTE: It is not necessary to create or make current to a context before
    // calling glXGetProcAddressARB
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 0;
    glXCreateContextAttribsARB = (glXCreateContextAttribsARBProc)
            glXGetProcAddressARB( (const GLubyte *) "glXCreateContextAttribsARB" );

    ctx = 0;

    // Install an X error handler so the application won't exit if GL 3.0
    // context allocation fails.
    //
    // Note this error handler is global.  All display connections in all threads
    // of a process use the same error handler, so be sure to guard against other
    // threads issuing X commands while this code is running.
    ctxErrorOccurred = false;
    int (*oldHandler)(Display*, XErrorEvent*) =
            XSetErrorHandler(&ctxErrorHandler);

    // Check for the GLX_ARB_create_context extension string and the function.
    // If either is not present, use GLX 1.3 context creation method.
    if ( !isExtensionSupported( glxExts, "GLX_ARB_create_context" ) ||
         !glXCreateContextAttribsARB )
    {
        printf( "glXCreateContextAttribsARB() not found"
                " ... using old-style GLX context\n" );
        ctx = glXCreateNewContext( display, bestFbc, GLX_RGBA_TYPE, 0, True );
    }

    // If it does, try to get a GL 3.0 context!
    else
    {
        int context_attribs[] =
        {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 0,
            //GLX_CONTEXT_FLAGS_ARB        , GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            None
        };

        printf( "Creating context\n" );
        ctx = glXCreateContextAttribsARB( display, bestFbc, 0,
                                          True, context_attribs );

        // Sync to ensure any errors generated are processed.
        XSync( display, False );
        if ( !ctxErrorOccurred && ctx )
            printf( "Created GL 3.0 context\n" );
        else
        {
            // Couldn't create GL 3.0 context.  Fall back to old-style 2.x context.
            // When a context version below 3.0 is requested, implementations will
            // return the newest context version compatible with OpenGL versions less
            // than version 3.0.
            // GLX_CONTEXT_MAJOR_VERSION_ARB = 1
            context_attribs[1] = 1;
            // GLX_CONTEXT_MINOR_VERSION_ARB = 0
            context_attribs[3] = 0;

            ctxErrorOccurred = false;

            printf( "Failed to create GL 3.0 context"
                    " ... using old-style GLX context\n" );
            ctx = glXCreateContextAttribsARB( display, bestFbc, 0,
                                              True, context_attribs );
        }
    }

    // Sync to ensure any errors generated are processed.
    XSync( display, False );

    // Restore the original error handler
    XSetErrorHandler( oldHandler );

    if ( ctxErrorOccurred || !ctx )
    {
        printf( "Failed to create an OpenGL context\n" );
        exit(1);
    }

    // Verifying that context is a direct context
    if ( ! glXIsDirect ( display, ctx ) )
    {
        printf( "Indirect GLX rendering context obtained\n" );
    }
    else
    {
        printf( "Direct GLX rendering context obtained\n" );
    }

    printf( "Making context current\n" );
    glXMakeCurrent( display, win, ctx );

    return 0;
}


namespace pangolin
{

// TODO: Actually call this at some point
void Terminate()
{
    glXMakeCurrent( display, 0, 0 );
    glXDestroyContext( display, ctx );

    XDestroyWindow( display, win );
    XFreeColormap( display, cmap );
    XCloseDisplay( display );
}

void ProcessX11Events()
{
    XEvent ev;
    while(XCheckWindowEvent(display,win,EVENT_MASKS,&ev))
    {
        switch(ev.type){
        case ConfigureNotify:
            if (pangolin::context->windowed_size[0] != ev.xconfigure.width
                    || pangolin::context->windowed_size[1] != ev.xconfigure.height) {
                pangolin::context->windowed_size[0] = ev.xconfigure.width;
                pangolin::context->windowed_size[1] = ev.xconfigure.height;
                printf("Size changed to: %d by %d\n", pangolin::context->windowed_size[0], pangolin::context->windowed_size[1]);
            }
            break;
        case ButtonPress:
            Terminate();
            pangolin::Quit();
        }
    }
    fflush(stdout);
}

void FinishFrame()
{
    RenderViews();
    PostRender();
    glXSwapBuffers ( display, win );
    ProcessX11Events();
}

void CreateWindowAndBind(std::string window_title, int w, int h )
{
    // Create Pangolin GL Context
    BindToContext(window_title);
    PangolinCommonInit();
    context->is_double_buffered = true;

    CreateX11Window(w, h);
    glewInit();

    // Process window events

}

void StartFullScreen() {
}

void StopFullScreen() {
}

void SetFullscreen(bool fullscreen)
{
    if( fullscreen != context->is_fullscreen )
    {
        if(fullscreen) {
            StartFullScreen();
        }else{
            StopFullScreen();
        }
        context->is_fullscreen = fullscreen;
    }
}
}

