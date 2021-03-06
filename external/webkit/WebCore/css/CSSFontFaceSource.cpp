

#include "config.h"
#include "CSSFontFaceSource.h"

#include "CachedFont.h"
#include "CSSFontFace.h"
#include "CSSFontSelector.h"
#include "DocLoader.h"
#include "FontCache.h"
#include "FontDescription.h"
#include "GlyphPageTreeNode.h"
#include "SimpleFontData.h"

#if ENABLE(SVG_FONTS)
#include "FontCustomPlatformData.h"
#include "HTMLNames.h"
#include "SVGFontData.h"
#include "SVGFontElement.h"
#include "SVGURIReference.h"
#endif

namespace WebCore {

CSSFontFaceSource::CSSFontFaceSource(const String& str, CachedFont* font)
    : m_string(str)
    , m_font(font)
    , m_face(0)
#if ENABLE(SVG_FONTS)
    , m_svgFontFaceElement(0)
#endif
{
    if (m_font)
        m_font->addClient(this);
}

CSSFontFaceSource::~CSSFontFaceSource()
{
    if (m_font)
        m_font->removeClient(this);
    pruneTable();
}

void CSSFontFaceSource::pruneTable()
{
    if (m_fontDataTable.isEmpty())
        return;
    HashMap<unsigned, SimpleFontData*>::iterator end = m_fontDataTable.end();
    for (HashMap<unsigned, SimpleFontData*>::iterator it = m_fontDataTable.begin(); it != end; ++it)
        GlyphPageTreeNode::pruneTreeCustomFontData(it->second);
    deleteAllValues(m_fontDataTable);
    m_fontDataTable.clear();
}

bool CSSFontFaceSource::isLoaded() const
{
    if (m_font)
        return m_font->isLoaded();
    return true;
}

bool CSSFontFaceSource::isValid() const
{
    if (m_font)
        return !m_font->errorOccurred();
    return true;
}

void CSSFontFaceSource::fontLoaded(CachedFont*)
{
    pruneTable();
    if (m_face)
        m_face->fontLoaded(this);
}

SimpleFontData* CSSFontFaceSource::getFontData(const FontDescription& fontDescription, bool syntheticBold, bool syntheticItalic, CSSFontSelector* fontSelector)
{
    // If the font hasn't loaded or an error occurred, then we've got nothing.
    if (!isValid())
        return 0;

#if ENABLE(SVG_FONTS)
    if (!m_font && !m_svgFontFaceElement) {
#else
    if (!m_font) {
#endif
        SimpleFontData* fontData = fontCache()->getCachedFontData(fontDescription, m_string);

        // We're local. Just return a SimpleFontData from the normal cache.
        return fontData;
    }

    // See if we have a mapping in our FontData cache.
    unsigned hashKey = fontDescription.computedPixelSize() << 2 | (syntheticBold ? 2 : 0) | (syntheticItalic ? 1 : 0);
    if (SimpleFontData* cachedData = m_fontDataTable.get(hashKey))
        return cachedData;

    OwnPtr<SimpleFontData> fontData;

    // If we are still loading, then we let the system pick a font.
    if (isLoaded()) {
        if (m_font) {
#if ENABLE(SVG_FONTS)
            if (m_font->isSVGFont()) {
                // For SVG fonts parse the external SVG document, and extract the <font> element.
                if (!m_font->ensureSVGFontData())
                    return 0;

                if (!m_externalSVGFontElement)
                    m_externalSVGFontElement = m_font->getSVGFontById(SVGURIReference::getTarget(m_string));

                if (!m_externalSVGFontElement)
                    return 0;

                SVGFontFaceElement* fontFaceElement = 0;

                // Select first <font-face> child
                for (Node* fontChild = m_externalSVGFontElement->firstChild(); fontChild; fontChild = fontChild->nextSibling()) {
                    if (fontChild->hasTagName(SVGNames::font_faceTag)) {
                        fontFaceElement = static_cast<SVGFontFaceElement*>(fontChild);
                        break;
                    }
                }

                if (fontFaceElement) {
                    if (!m_svgFontFaceElement) {
                        // We're created using a CSS @font-face rule, that means we're not associated with a SVGFontFaceElement.
                        // Use the imported <font-face> tag as referencing font-face element for these cases.
                        m_svgFontFaceElement = fontFaceElement;
                    }

                    SVGFontData* svgFontData = new SVGFontData(fontFaceElement);
                    fontData.set(new SimpleFontData(m_font->platformDataFromCustomData(fontDescription.computedPixelSize(), syntheticBold, syntheticItalic, fontDescription.renderingMode()), true, false, svgFontData));
                }
            } else
#endif
            {
                // Create new FontPlatformData from our CGFontRef, point size and ATSFontRef.
                if (!m_font->ensureCustomFontData())
                    return 0;

                fontData.set(new SimpleFontData(m_font->platformDataFromCustomData(fontDescription.computedPixelSize(), syntheticBold, syntheticItalic, fontDescription.renderingMode()), true, false));
            }
        } else {
#if ENABLE(SVG_FONTS)
            // In-Document SVG Fonts
            if (m_svgFontFaceElement) {
                SVGFontData* svgFontData = new SVGFontData(m_svgFontFaceElement);
                fontData.set(new SimpleFontData(FontPlatformData(fontDescription.computedPixelSize(), syntheticBold, syntheticItalic), true, false, svgFontData));
            }
#endif
        }
    } else {
        // Kick off the load now.
        if (DocLoader* docLoader = fontSelector->docLoader())
            m_font->beginLoadIfNeeded(docLoader);
        // FIXME: m_string is a URL so it makes no sense to pass it as a family name.
        SimpleFontData* tempData = fontCache()->getCachedFontData(fontDescription, m_string);
        if (!tempData)
            tempData = fontCache()->getLastResortFallbackFont(fontDescription);

        fontData.set(new SimpleFontData(tempData->platformData(), true, true));
    }

    m_fontDataTable.set(hashKey, fontData.get());
    return fontData.release();
}

}

