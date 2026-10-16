#include <cstring>

#include "rendergraph/material.h"
#include "rendergraph/materialshader.h"

using namespace rendergraph;

bool BaseMaterial::updateUniformsByteArray(QByteArray* buf) {
    // I6f: Qt may hand us a uniform buffer that does not contain this material's
    // values (new/rebuilt batch, other material's buffer). The old code copied
    // the cache only while the dirty flag was set and then cleared the flag, so
    // the buffer could keep stale values (wrong colour). Compare the cache with
    // the target instead of trusting the flag. The matrix (uniform 0, 64 bytes)
    // is owned by BaseMaterialShader::updateUniformData (state.combinedMatrix()),
    // so it is never overwritten here.
    constexpr qsizetype kMatrixBytes = 64;
    auto pThis = static_cast<Material*>(this);
    const bool wasDirty = pThis->clearUniformsCacheDirty();
    const qsizetype size = pThis->uniformsCache().size();
    if (size <= kMatrixBytes || buf->size() < size) {
        return wasDirty;
    }
    const char* src = pThis->uniformsCache().data() + kMatrixBytes;
    char* dst = buf->data() + kMatrixBytes;
    const size_t n = static_cast<size_t>(size - kMatrixBytes);
    if (std::memcmp(dst, src, n) != 0) {
        std::memcpy(dst, src, n);
        return true;
    }
    return wasDirty;
}

int BaseMaterial::compare(const QSGMaterial* other) const {
    auto pThis = static_cast<const Material*>(this);
    return pThis->compare(static_cast<const Material*>(other));
}

QSGMaterialShader* BaseMaterial::createShader(QSGRendererInterface::RenderMode) const {
    // Note: this looks like a leak but it isn't: we pass ownership to Qt.
    // Qt will cache and reuse the shader for all Material of the same type.
    // TODO make sure that RenderMode is always the same.
    auto pThis = static_cast<const Material*>(this);
    auto pShader = pThis->createShader().release();
    return pShader;
}
