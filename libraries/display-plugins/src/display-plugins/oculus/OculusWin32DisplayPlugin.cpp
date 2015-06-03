//
//  Created by Bradley Austin Davis on 2014/04/13.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "OculusWin32DisplayPlugin.h"

#include <memory>

#include <QMainWindow>
#include <QGLWidget>
#include <GLMHelpers.h>
#include <GlWindow.h>
#include <QEvent>
#include <QResizeEvent>

#include <OVR_CAPI_GL.h>

#include <PerfStat.h>

#include "plugins/PluginContainer.h"

#include "OculusHelpers.h"
#include <oglplus/opt/list_init.hpp>
#include <oglplus/shapes/vector.hpp>
#include <oglplus/opt/list_init.hpp>
#include <oglplus/shapes/obj_mesh.hpp>

#include "../OglplusHelpers.h"

#define DEFAULT_HMD_UI_ANGULAR_SIZE 72.0f

using oglplus::Framebuffer;
using oglplus::DefaultFramebuffer;

// A basic wrapper for constructing a framebuffer with a renderbuffer
// for the depth attachment and an undefined type for the color attachement
// This allows us to reuse the basic framebuffer code for both the Mirror
// FBO as well as the Oculus swap textures we will use to render the scene
// Though we don't really need depth at all for the mirror FBO, or even an
// FBO, but using one means I can just use a glBlitFramebuffer to get it onto
// the screen.
template <typename C = GLuint, typename D = GLuint>
struct FramebufferWrapper {
    uvec2       size;
    Framebuffer fbo;
    C           color{ 0 };
    D           depth{ 0 };

    virtual ~FramebufferWrapper() {
    }

    FramebufferWrapper() {}

    virtual void Init(const uvec2 & size) {
        this->size = size;
        initColor();
        initDepth();
        initDone();
    }

    template <typename F>
    void Bound(F f) {
        Bound(GL_FRAMEBUFFER, f);
    }

    template <typename F>
    void Bound(GLenum target, F f) {
        glBindFramebuffer(target, oglplus::GetName(fbo));
        onBind(target);
        f();
        onUnbind(target);
        glBindFramebuffer(target, 0);
    }

    void Viewport() {
        glViewport(0, 0, size.x, size.y);
    }

protected:
    virtual void onBind(GLenum target) {}
    virtual void onUnbind(GLenum target) {}


    virtual void initDepth() {
        glGenRenderbuffers(1, &depth);
        assert(depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, size.x, size.y);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    virtual void initColor() = 0;
    virtual void initDone() = 0;
};



// A base class for FBO wrappers that need to use the Oculus C
// API to manage textures via ovrHmd_CreateSwapTextureSetGL,
// ovrHmd_CreateMirrorTextureGL, etc
template <typename C>
struct RiftFramebufferWrapper : public FramebufferWrapper<C> {
    ovrHmd hmd;
    RiftFramebufferWrapper(const ovrHmd & hmd) : hmd(hmd) {};

    void Resize(const uvec2 & size) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oglplus::GetName(fbo));
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        this->size = size;
        initColor();
        initDone();
    }

protected:
    virtual void initDepth() override {
    }
};

// A wrapper for constructing and using a swap texture set,
// where each frame you draw to a texture via the FBO,
// then submit it and increment to the next texture.
// The Oculus SDK manages the creation and destruction of
// the textures
struct SwapFramebufferWrapper : public RiftFramebufferWrapper<ovrSwapTextureSet*> {
    SwapFramebufferWrapper(const ovrHmd & hmd)
        : RiftFramebufferWrapper(hmd) {
    }
    ~SwapFramebufferWrapper() {
        if (color) {
            ovrHmd_DestroySwapTextureSet(hmd, color);
            color = nullptr;
        }
    }

    void Increment() {
        ++color->CurrentIndex;
        color->CurrentIndex %= color->TextureCount;
    }

protected:
    virtual void initColor() override {
        if (color) {
            ovrHmd_DestroySwapTextureSet(hmd, color);
            color = nullptr;
        }

        if (!OVR_SUCCESS(ovrHmd_CreateSwapTextureSetGL(hmd, GL_RGBA, size.x, size.y, &color))) {
            qFatal("Unable to create swap textures");
        }

        for (int i = 0; i < color->TextureCount; ++i) {
            ovrGLTexture& ovrTex = (ovrGLTexture&)color->Textures[i];
            glBindTexture(GL_TEXTURE_2D, ovrTex.OGL.TexId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    virtual void initDone() override {
    }

    virtual void onBind(GLenum target) {
        ovrGLTexture& tex = (ovrGLTexture&)(color->Textures[color->CurrentIndex]);
        glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex.OGL.TexId, 0);
    }

    virtual void onUnbind(GLenum target) {
        glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
};


// We use a FBO to wrap the mirror texture because it makes it easier to
// render to the screen via glBlitFramebuffer
struct MirrorFramebufferWrapper : public RiftFramebufferWrapper<ovrGLTexture*> {
    float                   targetAspect;
    MirrorFramebufferWrapper(const ovrHmd & hmd)
        : RiftFramebufferWrapper(hmd) {
    }
    ~MirrorFramebufferWrapper() {
        if (color) {
            ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)color);
            color = nullptr;
        }
    }

private:
    void initColor() override {
        if (color) {
            ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)color);
            color = nullptr;
        }
        ovrResult result = ovrHmd_CreateMirrorTextureGL(hmd, GL_RGBA, size.x, size.y, (ovrTexture**)&color);
        Q_ASSERT(OVR_SUCCESS(result));
    }

    void initDone() override {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oglplus::GetName(fbo));
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->OGL.TexId, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

};

const QString OculusWin32DisplayPlugin::NAME("Oculus Rift");

const QString & OculusWin32DisplayPlugin::getName() {
    return NAME;
}

bool OculusWin32DisplayPlugin::isSupported() {
    if (!OVR_SUCCESS(ovr_Initialize(nullptr))) {
        return false;
    }
    bool result = false;
    if (ovrHmd_Detect() > 0) {
        result = true;
    }
    ovr_Shutdown();
    return result;
}

ovrLayerEyeFov& OculusWin32DisplayPlugin::getSceneLayer() {
    return _sceneLayer;
}

//static gpu::TexturePointer _texture;

void OculusWin32DisplayPlugin::activate(PluginContainer * container) {
    if (!OVR_SUCCESS(ovr_Initialize(nullptr))) {
        Q_ASSERT(false);
        qFatal("Failed to Initialize SDK");
    }
    if (!OVR_SUCCESS(ovrHmd_Create(0, &_hmd))) {
        Q_ASSERT(false);
        qFatal("Failed to acquire HMD");
    }
    // Parent class relies on our _hmd intialization, so it must come after that.
    ovrLayerEyeFov& sceneLayer = getSceneLayer();
    memset(&sceneLayer, 0, sizeof(ovrLayerEyeFov));
    sceneLayer.Header.Type = ovrLayerType_EyeFov;
    sceneLayer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
    ovr_for_each_eye([&](ovrEyeType eye) {
        ovrFovPort & fov = sceneLayer.Fov[eye] = _eyeRenderDescs[eye].Fov;
        ovrSizei & size = sceneLayer.Viewport[eye].Size = ovrHmd_GetFovTextureSize(_hmd, eye, fov, 1.0f);
        sceneLayer.Viewport[eye].Pos = { eye == ovrEye_Left ? 0 : size.w, 0 };
    });

    //ovrLayerQuad& uiLayer = getUiLayer();
    //memset(&uiLayer, 0, sizeof(ovrLayerQuad));
    //uiLayer.Header.Type = ovrLayerType_QuadInWorld;
    //uiLayer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft | ovrLayerFlag_HighQuality;
    //uiLayer.QuadPoseCenter.Orientation = { 0, 0, 0, 1 };
    //uiLayer.QuadPoseCenter.Position = { 0, 0, -1 };

    OculusBaseDisplayPlugin::activate(container);
}

void OculusWin32DisplayPlugin::customizeContext(PluginContainer * container) {
    OculusBaseDisplayPlugin::customizeContext(container);
    
    //_texture = DependencyManager::get<TextureCache>()->
    //    getImageTexture(PathUtils::resourcesPath() + "/images/cube_texture.png");

    _uiSurface = loadSphereSection(_program, glm::radians(DEFAULT_HMD_UI_ANGULAR_SIZE), aspect(getCanvasSize()));

    uvec2 mirrorSize = toGlm(_widget->geometry().size());
    _mirrorFbo = MirrorFboPtr(new MirrorFramebufferWrapper(_hmd));
    _mirrorFbo->Init(mirrorSize);


    uvec2 swapSize = toGlm(getRecommendedFramebufferSize());
    _sceneFbo = SwapFboPtr(new SwapFramebufferWrapper(_hmd));
    _sceneFbo->Init(swapSize);

    // We're rendering both eyes to the same texture, so only one of the 
    // pointers is populated
    ovrLayerEyeFov& sceneLayer = getSceneLayer();
    sceneLayer.ColorTexture[0] = _sceneFbo->color;
    // not needed since the structure was zeroed on init, but explicit
    sceneLayer.ColorTexture[1] = nullptr;
}

void OculusWin32DisplayPlugin::deactivate() {
    makeCurrent();
    _sceneFbo.reset();
    _mirrorFbo.reset();
    _uiSurface.reset();
    doneCurrent();

    OculusBaseDisplayPlugin::deactivate();

    ovrHmd_Destroy(_hmd);
    _hmd = nullptr;
    ovr_Shutdown();
}

void OculusWin32DisplayPlugin::display(
    GLuint sceneTexture, const glm::uvec2& sceneSize,
    GLuint overlayTexture, const glm::uvec2& overlaySize) {
    using namespace oglplus;
    bool wasActive = PerformanceTimer::isActive();
    PerformanceTimer::setActive(true);
    PerformanceTimer("OculusDisplayAndSwap");
    
    wglSwapIntervalEXT(0);

    {

    }
    _sceneFbo->Bound([&] {
        auto size = _sceneFbo->size;
        Context::Viewport(size.x, size.y);

        glClearColor(0, 0, 0, 0);
        Context::Clear().ColorBuffer();

        _program->Bind();
        Mat4Uniform(*_program, "Projection").Set(mat4());
        Mat4Uniform(*_program, "ModelView").Set(mat4());
        glBindTexture(GL_TEXTURE_2D, sceneTexture);
        _plane->Use();
        _plane->Draw();

        
        Context::Enable(Capability::Blend);
        glBindTexture(GL_TEXTURE_2D, overlayTexture);
        //glBindTexture(GL_TEXTURE_2D, gpu::GLBackend::getTextureID(_texture));
        for_each_eye([&](Eye eye) {
            Context::Viewport(eye == Left ? 0 : size.x / 2, 0, size.x / 2, size.y);
            Mat4Uniform(*_program, "Projection").Set(getProjection(eye, mat4()));
            Mat4Uniform(*_program, "ModelView").Set(glm::scale(glm::inverse(getModelview(eye, mat4())), vec3(1)));
            _uiSurface->Use();
            _uiSurface->Draw();
        });
        Context::Disable(Capability::Blend);
    });

    /*
    ovrLayerQuad& uiLayer = getUiLayer();
    if (nullptr == uiLayer.ColorTexture || overlaySize != _uiFbo->size) {
        _uiFbo->Resize(overlaySize);
        uiLayer.ColorTexture = _uiFbo->color;
        uiLayer.Viewport.Size.w = overlaySize.x;
        uiLayer.Viewport.Size.h = overlaySize.y;
        float overlayAspect = aspect(overlaySize);
        uiLayer.QuadSize.x = 1.0f;
        uiLayer.QuadSize.y = 1.0f / overlayAspect;
    }

    _uiFbo->Bound([&] {
        Q_ASSERT(0 == glGetError());
        using namespace oglplus;
        Context::Viewport(_uiFbo->size.x, _uiFbo->size.y);
        glClearColor(0, 0, 0, 0);
        Context::Clear().ColorBuffer();

        _program->Bind();
        glBindTexture(GL_TEXTURE_2D, overlayTexture);
        _plane->Use();
        _plane->Draw();
        Q_ASSERT(0 == glGetError());
    });
    */

    ovrLayerEyeFov& sceneLayer = getSceneLayer(); 
    ovr_for_each_eye([&](ovrEyeType eye) {
        sceneLayer.RenderPose[eye] = _eyePoses[eye];
    });


    auto windowSize = toGlm(getDeviceSize());

    /* 
       Two alternatives for mirroring to the screen, the first is to copy our own composited
       scene to the window framebuffer, before distortion.  Note this only works if we're doing
       ui compositing ourselves, and not relying on the Oculus SDK compositor (or we don't want 
       the UI visible in the output window (unlikely).  This should be done before 
    */
    // _sceneFbo->Increment or we're be using the wrong texture
    //_sceneFbo->Bound(GL_READ_FRAMEBUFFER, [&] {
    //    glBlitFramebuffer(
    //        0, 0, _sceneFbo->size.x, _sceneFbo->size.y,
    //        0, 0, windowSize.x, _mirrorFbo.y,
    //        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    //});

    {
        PerformanceTimer("OculusSubmit");
        ovrLayerHeader* layers = &sceneLayer.Header;
        ovrResult result = ovrHmd_SubmitFrame(_hmd, _frameIndex, nullptr, &layers, 1);
    }
    _sceneFbo->Increment();

    /* 
        The other alternative for mirroring is to use the Oculus mirror texture support, which 
        will contain the post-distorted and fully composited scene regardless of how many layers 
        we send.
    */
    auto mirrorSize = _mirrorFbo->size;
    _mirrorFbo->Bound(GL_READ_FRAMEBUFFER, [&] {
        glBlitFramebuffer(
            0, mirrorSize.y, mirrorSize.x, 0,
            0, 0, windowSize.x, windowSize.y,
            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    });

    ++_frameIndex;
//    swapBuffers();
    PerformanceTimer::setActive(wasActive);
    if (0 == (_frameIndex % (75 * 5))) {
        auto record1 = PerformanceTimer::getTimerRecord("OculusDisplayAndSwap");
        auto record2 = PerformanceTimer::getTimerRecord("OculusSubmit");
        qDebug() << "Average display and submit: " << record1.getAverage();
        qDebug() << "Average submit: " << record2.getAverage();
        qDebug() << "Diff " << record1.getAverage() - record2.getAverage();
    }
}

// Pass input events on to the application
bool OculusWin32DisplayPlugin::eventFilter(QObject* receiver, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        QResizeEvent* resizeEvent = static_cast<QResizeEvent*>(event);
        qDebug() << resizeEvent->size().width() << " x " << resizeEvent->size().height();
        auto newSize = toGlm(resizeEvent->size());
        makeCurrent();
        _mirrorFbo->Resize(newSize);
        doneCurrent();
    }
    return OculusBaseDisplayPlugin::eventFilter(receiver, event);
}


void OculusWin32DisplayPlugin::finishFrame() {
    doneCurrent();
};
