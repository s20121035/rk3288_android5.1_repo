/*
 * Copyright 2013 The Android Open Source Project
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


#ifndef SF_GLES20RENDERENGINE_H_
#define SF_GLES20RENDERENGINE_H_

typedef unsigned int uint32_t;

#include <stdint.h>
#include <sys/types.h>

#include <GLES2/gl2.h>
#include <Transform.h>

#include "Program.h"
#include "Mesh.h"
#include "RenderEngine.h"
#include "ProgramCache.h"
#include "Description.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------

class String8;
class Mesh;
class Texture;

class GLES20RenderEngine : public RenderEngine {
    GLuint mProtectedTexName;
    GLint mMaxViewportDims[2];
    GLint mMaxTextureSize;
    GLuint mVpWidth;
    GLuint mVpHeight;
    GLuint VRMeshBuffer;
    GLuint tname, name;
    GLuint leftFbo, leftTex;
    GLuint rightFbo, rightTex;
    GLuint checkLeftTex,checkRightTex;
    GLuint checkLeftFBO,checkRightFBO;
    GLboolean is3dApp;
    GLuint checkRate;
    GLuint checkBegin;
    bool useRightFBO;
    void * context;
    uint32_t * leftCheck;
    uint32_t * rightCheck;

    struct VRInfoTable {
        GLuint VRMeshBuffer;
        GLuint VRMeshCheckBuffer;
        
        GLuint leftFbo, leftTex;
        GLuint rightFbo, rightTex;
        
        GLuint checkLeftTex,checkLeftFBO;
        GLuint checkRightTex,checkRightFBO;
        
        uint32_t * checkLeftPtr;
        uint32_t * checkRightPtr;

        GLuint fboWidth,fboHeight;
        
        GLboolean is3dApp;
    } mVRInfoTable;
    
    struct Group {
        GLuint texture;
        GLuint fbo;
        GLuint width;
        GLuint height;
        mat4 colorTransform;
    };

    Description mState;
    Vector<Group> mGroupStack;

    virtual void bindImageAsFramebuffer(EGLImageKHR image,
            uint32_t* texName, uint32_t* fbName, uint32_t* status);
    virtual void unbindFramebuffer(uint32_t texName, uint32_t fbName);

public:
    GLES20RenderEngine();

protected:
    virtual ~GLES20RenderEngine();

    virtual void dump(String8& result);
    virtual void setViewportAndProjection(size_t vpw, size_t vph,
            Rect sourceCrop, size_t hwh, bool yswap, Transform::orientation_flags rotation);
    virtual void setupLayerBlending(bool premultipliedAlpha, bool opaque, int alpha);
    virtual void setupDimLayerBlending(int alpha);
    virtual void setupLayerTexturing(const Texture& texture);
    virtual void setupLayerBlackedOut();
    virtual void setupFillWithColor(float r, float g, float b, float a);
    virtual void disableTexturing();
    virtual void disableBlending();

    virtual void drawMesh(const Mesh& mesh);

#ifdef ENABLE_VR
    virtual void initVRInfoTable();
    virtual void drawMeshLeftEye();
    virtual void drawMeshRightEye();
    
    virtual void drawMeshLeftFBO(const Mesh& mesh);
    virtual void drawMeshRightFBO(const Mesh& mesh);

    virtual void drawMeshLeftCheckFBO(const Mesh& mesh);
    virtual void drawMeshRightCheckFBO(const Mesh& mesh);
    
    virtual bool isSimilaryImages(const uint32_t* frame1,const uint32_t * frame2);
    virtual void enableShaderTexArray();
    virtual void enableShaderVerArray(int mode);
    virtual GLuint genCheckFBOMeshBuffer();
    virtual GLuint genVRMeshBuffer(float halfWidth,float halfHeight);
    virtual vec2 genDeformTex(vec2 tex,float k1,float k2);
    
    virtual void enableRightFBO(bool key);
    virtual bool checkSimilarity();
    
    virtual bool checkVRPropertyChanged();
    virtual bool testSimilartiy();
    virtual void readPixelsForSimilarity();
    virtual void beginGroup(const mat4& colorTransform,int mode);
    virtual void endGroup(int mode);
    
#else
    virtual void beginGroup(const mat4& colorTransform);
    virtual void endGroup();
#endif

    virtual size_t getMaxTextureSize() const;
    virtual size_t getMaxViewportDims() const;
};

// ---------------------------------------------------------------------------
}; // namespace android
// ---------------------------------------------------------------------------

#endif /* SF_GLES20RENDERENGINE_H_ */
