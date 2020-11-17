//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once

#include "../Core/Object.h"
#include "../Graphics/Drawable.h"
#include "../RenderPipeline/SceneBatchCollectorCallback.h"
#include "../Scene/Serializable.h"

namespace Urho3D
{

class Camera;
class RenderPath;
class RenderSurface;
class Scene;
class XMLFile;
class View;
class Viewport;
class RenderPipelineViewport;
class ShadowMapAllocator;

struct RenderPipelineSettings
{
    bool deferred_{};
    bool gammaCorrection_{};
};

///
class URHO3D_API RenderPipeline : public Serializable, public SceneBatchCollectorCallback
{
    URHO3D_OBJECT(RenderPipeline, Serializable);

public:
    /// Construct with defaults.
    RenderPipeline(Context* context);
    /// Destruct.
    ~RenderPipeline() override;

    /// Register object with the engine.
    static void RegisterObject(Context* context);
    /// Apply attribute changes that can not be applied immediately. Called after scene load or a network update.
    void ApplyAttributes() override;

    /// Define all dependencies.
    bool Define(RenderSurface* renderTarget, Viewport* viewport);

    /// Update.
    void Update(const FrameInfo& frameInfo);

    /// Render.
    void Render();

protected:
    unsigned GetNumThreads() const { return numThreads_; }
    void PostTask(std::function<void(unsigned)> task);
    void CompleteTasks();

    //void ClearViewport(ClearTargetFlags flags, const Color& color, float depth, unsigned stencil) override;
    void CollectDrawables(ea::vector<Drawable*>& drawables, Camera* camera, DrawableFlags flags);
    bool HasShadow(Light* light);
    ShadowMap GetTemporaryShadowMap(const IntVector2& size);

    SharedPtr<PipelineState> CreatePipelineState(
        const ScenePipelineStateKey& key, const ScenePipelineStateContext& ctx) override;
    /// Return new or existing pipeline state for deferred light volume.
    SharedPtr<PipelineState> CreateLightVolumePipelineState(SceneLight* sceneLight, Geometry* lightGeometry) override;

private:
    RenderPipelineSettings settings_;

    Graphics* graphics_{};
    Renderer* renderer_{};
    WorkQueue* workQueue_{};

    Scene* scene_{};
    Camera* camera_{};
    Octree* octree_{};

    unsigned numThreads_{};
    unsigned numDrawables_{};

    FrameInfo frameInfo_{};

    SharedPtr<RenderPipelineViewport> viewport_;
    SharedPtr<ShadowMapAllocator> shadowMapAllocator_;
};

}