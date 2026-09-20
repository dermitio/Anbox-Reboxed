#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile


SOURCE = r'''bool ColorBufferGl::bindToTexture() {
    if (!m_eglImage) {
        return false;
    }

    RenderThreadInfoGl* const tInfo = RenderThreadInfoGl::get();
    if (!tInfo) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Render thread GL not available.";
    }

    if (!tInfo->currContext.get()) {
        return false;
    }

    if (tInfo->currContext->clientVersion() > GLESApi_CM) {
        s_gles2.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
    } else {
        s_gles1.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
    }
    return true;
}

bool ColorBufferGl::bindToTexture2() {
    if (!m_eglImage) {
        return false;
    }

    s_gles2.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
    return true;
}
'''

PROGRAM_BINARY_SOURCE = r'''GL_APICALL void GL_APIENTRY glProgramBinary(GLuint program, GLenum binaryFormat, const void * binary, GLsizei length) {
    GET_CTX_V2();
    if (ctx->shareGroup().get()) {
        const GLuint globalProgramName = ctx->shareGroup()->getGlobalName(NamedObjectType::SHADER_OR_PROGRAM, program);
        SET_ERROR_IF(globalProgramName == 0, GL_INVALID_VALUE);

        auto objData =
            ctx->shareGroup()->getObjectData(NamedObjectType::SHADER_OR_PROGRAM, program);
        SET_ERROR_IF(!objData, GL_INVALID_OPERATION);
        SET_ERROR_IF(objData->getDataType() != PROGRAM_DATA, GL_INVALID_OPERATION);

        ProgramData* programData = (ProgramData*)objData;

        ctx->dispatcher().glProgramBinary(globalProgramName, binaryFormat, binary, length);

        GLint linkStatus = GL_FALSE;
        ctx->dispatcher().glGetProgramiv(globalProgramName, GL_LINK_STATUS, &linkStatus);

        programData->setHostLinkStatus(linkStatus);
        programData->setLinkStatus(linkStatus);

        GLsizei infoLogLength = 0;
        ctx->dispatcher().glGetProgramiv(globalProgramName, GL_INFO_LOG_LENGTH, &infoLogLength);

        if (infoLogLength > 0) {
            std::vector<GLchar> infoLog(infoLogLength);
            ctx->dispatcher().glGetProgramInfoLog(globalProgramName, infoLogLength, &infoLogLength,
                                                  infoLog.data());

            if (infoLogLength) {
                infoLog.resize(infoLogLength);
                programData->setInfoLog(infoLog.data());
            }
        }
    }
}

'''


def git_apply(root, *arguments):
    return subprocess.run(
        ["git", "apply", *arguments], cwd=root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    patch = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anbox-gfxstream-host-patch-") as root:
        root = pathlib.Path(root)
        source = root / "host/gl/ColorBufferGl.cpp"
        source.parent.mkdir(parents=True)
        source.write_text(SOURCE, encoding="utf-8")
        program_binary_source = (
            root / "host/gl/glestranslator/GLES_V2/GLESv30Imp.cpp")
        program_binary_source.parent.mkdir(parents=True)
        # Keep the synthetic function near the pinned source line number so
        # git-apply exercises the canonical hunk without excessive offset.
        program_binary_source.write_text(
            "\n" * 981 + PROGRAM_BINARY_SOURCE, encoding="utf-8")

        result = git_apply(root, "--check", str(patch))
        if result.returncode:
            raise RuntimeError(result.stderr)
        result = git_apply(root, str(patch))
        if result.returncode:
            raise RuntimeError(result.stderr)

        patched = source.read_text(encoding="utf-8")
        assert patched.count("    waitSync();") == 2
        patched_program_binary = program_binary_source.read_text(encoding="utf-8")
        assert "ctx->dispatcher().glProgramBinary(" in patched_program_binary
        assert "ProgramData* programData" not in patched_program_binary
        assert "programData->setLinkStatus(linkStatus);" not in patched_program_binary
        assert git_apply(root, "--reverse", "--check", str(patch)).returncode == 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
