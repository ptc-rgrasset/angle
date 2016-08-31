//
// Copyright (c) 2002-2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include "compiler/translator/TranslatorGLSL.h"

#include "angle_gl.h"
#include "compiler/translator/BuiltInFunctionEmulatorGLSL.h"
#include "compiler/translator/EmulatePrecision.h"
#include "compiler/translator/ExtensionGLSL.h"
#include "compiler/translator/OutputGLSL.h"
#include "compiler/translator/RewriteTexelFetchOffset.h"
#include "compiler/translator/VersionGLSL.h"

TranslatorGLSL::TranslatorGLSL(sh::GLenum type,
                               ShShaderSpec spec,
                               ShShaderOutput output)
    : TCompiler(type, spec, output) {
}

void TranslatorGLSL::initBuiltInFunctionEmulator(BuiltInFunctionEmulator *emu, int compileOptions)
{
    if (compileOptions & SH_EMULATE_BUILT_IN_FUNCTIONS)
    {
        InitBuiltInFunctionEmulatorForGLSLWorkarounds(emu, getShaderType());
    }

    int targetGLSLVersion = ShaderOutputTypeToGLSLVersion(getOutputType());
    InitBuiltInFunctionEmulatorForGLSLMissingFunctions(emu, getShaderType(), targetGLSLVersion);
}

void TranslatorGLSL::translate(TIntermNode *root, int compileOptions)
{
    TInfoSinkBase& sink = getInfoSink().obj;

    // Write GLSL version.
    writeVersion(root);

    // Write extension behaviour as needed
    writeExtensionBehavior(root);

    // Write pragmas after extensions because some drivers consider pragmas
    // like non-preprocessor tokens.
    writePragma(compileOptions);

    // If flattening the global invariant pragma, write invariant declarations for built-in
    // variables. It should be harmless to do this twice in the case that the shader also explicitly
    // did this. However, it's important to emit invariant qualifiers only for those built-in
    // variables that are actually used, to avoid affecting the behavior of the shader.
    if ((compileOptions & SH_FLATTEN_PRAGMA_STDGL_INVARIANT_ALL) && getPragma().stdgl.invariantAll)
    {
        collectVariables(root);

        switch (getShaderType())
        {
            case GL_VERTEX_SHADER:
                sink << "invariant gl_Position;\n";

                // gl_PointSize should be declared invariant in both ESSL 1.00 and 3.00 fragment
                // shaders if it's statically referenced.
                conditionallyOutputInvariantDeclaration("gl_PointSize");
                break;
            case GL_FRAGMENT_SHADER:
                // The preprocessor will reject this pragma if it's used in ESSL 3.00 fragment
                // shaders, so we can use simple logic to determine whether to declare these
                // variables invariant.
                conditionallyOutputInvariantDeclaration("gl_FragCoord");
                conditionallyOutputInvariantDeclaration("gl_PointCoord");
                break;
            default:
                // Currently not reached, but leave this in for future expansion.
                ASSERT(false);
                break;
        }
    }

    if ((compileOptions & SH_REWRITE_TEXELFETCHOFFSET_TO_TEXELFETCH) != 0)
    {
        sh::RewriteTexelFetchOffset(root, getTemporaryIndex(), getSymbolTable(),
                                    getShaderVersion());
    }

    bool precisionEmulation = getResources().WEBGL_debug_shader_precision && getPragma().debugShaderPrecision;

    if (precisionEmulation)
    {
        EmulatePrecision emulatePrecision(getSymbolTable(), getShaderVersion());
        root->traverse(&emulatePrecision);
        emulatePrecision.updateTree();
        emulatePrecision.writeEmulationHelpers(sink, getShaderVersion(), getOutputType());
    }

    // Write emulated built-in functions if needed.
    if (!getBuiltInFunctionEmulator().IsOutputEmpty())
    {
        sink << "// BEGIN: Generated code for built-in function emulation\n\n";
        sink << "#define webgl_emu_precision\n\n";
        getBuiltInFunctionEmulator().OutputEmulatedFunctions(sink);
        sink << "// END: Generated code for built-in function emulation\n\n";
    }

    // Write array bounds clamping emulation if needed.
    getArrayBoundsClamper().OutputClampingFunctionDefinition(sink);

    // Declare gl_FragColor and glFragData as webgl_FragColor and webgl_FragData
    // if it's core profile shaders and they are used.
    if (getShaderType() == GL_FRAGMENT_SHADER)
    {
        const bool mayHaveESSL1SecondaryOutputs =
            IsExtensionEnabled(getExtensionBehavior(), "GL_EXT_blend_func_extended") &&
            getShaderVersion() == 100;
        const bool declareGLFragmentOutputs = IsGLSL130OrNewer(getOutputType());

        bool hasGLFragColor          = false;
        bool hasGLFragData           = false;
        bool hasGLSecondaryFragColor = false;
        bool hasGLSecondaryFragData  = false;

        for (const auto &outputVar : outputVariables)
        {
            if (declareGLFragmentOutputs)
            {
                if (outputVar.name == "gl_FragColor")
                {
                    ASSERT(!hasGLFragColor);
                    hasGLFragColor = true;
                    continue;
                }
                else if (outputVar.name == "gl_FragData")
                {
                    ASSERT(!hasGLFragData);
                    hasGLFragData = true;
                    continue;
                }
            }
            if (mayHaveESSL1SecondaryOutputs)
            {
                if (outputVar.name == "gl_SecondaryFragColorEXT")
                {
                    ASSERT(!hasGLSecondaryFragColor);
                    hasGLSecondaryFragColor = true;
                    continue;
                }
                else if (outputVar.name == "gl_SecondaryFragDataEXT")
                {
                    ASSERT(!hasGLSecondaryFragData);
                    hasGLSecondaryFragData = true;
                    continue;
                }
            }
        }
        ASSERT(!((hasGLFragColor || hasGLSecondaryFragColor) &&
                 (hasGLFragData || hasGLSecondaryFragData)));
        if (hasGLFragColor)
        {
            sink << "out vec4 webgl_FragColor;\n";
        }
        if (hasGLFragData)
        {
            sink << "out vec4 webgl_FragData[gl_MaxDrawBuffers];\n";
        }
        if (hasGLSecondaryFragColor)
        {
            sink << "out vec4 angle_SecondaryFragColor;\n";
        }
        if (hasGLSecondaryFragData)
        {
            sink << "out vec4 angle_SecondaryFragData[" << getResources().MaxDualSourceDrawBuffers
                 << "];\n";
        }
    }

    if (getShaderType() == GL_COMPUTE_SHADER && isComputeShaderLocalSizeDeclared())
    {
        const sh::WorkGroupSize &localSize = getComputeShaderLocalSize();
        sink << "layout (local_size_x=" << localSize[0] << ", local_size_y=" << localSize[1]
             << ", local_size_z=" << localSize[2] << ") in;\n";
    }

    // Write translated shader.
    TOutputGLSL outputGLSL(sink,
                           getArrayIndexClampingStrategy(),
                           getHashFunction(),
                           getNameMap(),
                           getSymbolTable(),
                           getShaderVersion(),
                           getOutputType());
    root->traverse(&outputGLSL);
}

bool TranslatorGLSL::shouldFlattenPragmaStdglInvariantAll()
{
    // Required when outputting to any GLSL version greater than 1.20, but since ANGLE doesn't
    // translate to that version, return true for the next higher version.
    return IsGLSL130OrNewer(getOutputType());
}

void TranslatorGLSL::writeVersion(TIntermNode *root)
{
    TVersionGLSL versionGLSL(getShaderType(), getPragma(), getOutputType());
    root->traverse(&versionGLSL);
    int version = versionGLSL.getVersion();
    // We need to write version directive only if it is greater than 110.
    // If there is no version directive in the shader, 110 is implied.
    if (version > 110)
    {
        TInfoSinkBase& sink = getInfoSink().obj;
        sink << "#version " << version << "\n";
    }
}

void TranslatorGLSL::writeExtensionBehavior(TIntermNode *root)
{
    TInfoSinkBase& sink = getInfoSink().obj;
    const TExtensionBehavior& extBehavior = getExtensionBehavior();
    for (const auto &iter : extBehavior)
    {
        if (iter.second == EBhUndefined)
        {
            continue;
        }

        if (getOutputType() == SH_GLSL_COMPATIBILITY_OUTPUT)
        {
            // For GLSL output, we don't need to emit most extensions explicitly,
            // but some we need to translate in GL compatibility profile.
            if (iter.first == "GL_EXT_shader_texture_lod")
            {
                sink << "#extension GL_ARB_shader_texture_lod : " << getBehaviorString(iter.second)
                     << "\n";
            }

            if (iter.first == "GL_EXT_draw_buffers")
            {
                sink << "#extension GL_ARB_draw_buffers : " << getBehaviorString(iter.second)
                     << "\n";
            }
        }
    }

    // GLSL ES 3 explicit location qualifiers need to use an extension before GLSL 330
    if (getShaderVersion() >= 300 && getOutputType() < SH_GLSL_330_CORE_OUTPUT)
    {
        sink << "#extension GL_ARB_explicit_attrib_location : require\n";
    }

    // Need to enable gpu_shader5 to have index constant sampler array indexing
    if (getOutputType() != SH_ESSL_OUTPUT && getOutputType() < SH_GLSL_400_CORE_OUTPUT)
    {
        sink << "#extension GL_ARB_gpu_shader5 : ";

        // Don't use "require" on WebGL 1 to avoid breaking WebGL on drivers that silently
        // support index constant sampler array indexing, but don't have the extension.
        if (getShaderVersion() >= 300)
        {
            sink << "require\n";
        }
        else
        {
            sink << "enable\n";
        }
    }

    TExtensionGLSL extensionGLSL(getOutputType());
    root->traverse(&extensionGLSL);

    for (const auto &ext : extensionGLSL.getEnabledExtensions())
    {
        sink << "#extension " << ext << " : enable\n";
    }
    for (const auto &ext : extensionGLSL.getRequiredExtensions())
    {
        sink << "#extension " << ext << " : require\n";
    }
}

void TranslatorGLSL::conditionallyOutputInvariantDeclaration(const char *builtinVaryingName)
{
    if (isVaryingDefined(builtinVaryingName))
    {
        TInfoSinkBase &sink = getInfoSink().obj;
        sink << "invariant " << builtinVaryingName << ";\n";
    }
}
