/*
 * Copyright (c) 2022, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Julian Offenhäuser <offenhaeuser@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AntiAliasingPainter.h>
#include <LibPDF/CommonNames.h>
#include <LibPDF/Fonts/Type1Font.h>

namespace PDF {

static bool is_standard_latin_font(FlyString const& font)
{
    return font.is_one_of(
        "Times-Roman",
        "Helvetica",
        "Courier",
        "Times-Bold",
        "Helvetica-Bold",
        "Courier-Bold",
        "Times-Italic",
        "Helvetica-Oblique",
        "Courier-Oblique",
        "Times-BoldItalic",
        "Helvetica-BoldOblique",
        "Courier-BoldOblique");
}

PDFErrorOr<Type1Font::Data> Type1Font::parse_data(Document* document, NonnullRefPtr<DictObject> dict)
{
    // FIXME: "Required except for the standard 14 fonts"...
    //        "Beginning with PDF 1.5, the special treatment given to the standard 14
    //        fonts is deprecated. [...] For backwards capability, conforming readers
    //        shall still provide the special treatment identifier for the standard
    //        14 fonts."

    RefPtr<Encoding> encoding;

    if (dict->contains(CommonNames::Encoding)) {
        auto encoding_object = MUST(dict->get_object(document, CommonNames::Encoding));
        encoding = TRY(Encoding::from_object(document, encoding_object));
    } else {
        auto base_font = MUST(dict->get_name(document, CommonNames::BaseFont))->name();
        if (is_standard_latin_font(base_font)) {
            // FIXME: The spec doesn't specify what the encoding should be in this case
            encoding = Encoding::standard_encoding();
        }
        // Otherwise, use the built-in encoding of the font program.
    }

    RefPtr<StreamObject> to_unicode;
    if (dict->contains(CommonNames::ToUnicode))
        to_unicode = MUST(dict->get_stream(document, CommonNames::ToUnicode));

    auto first_char = dict->get_value(CommonNames::FirstChar).get<int>();
    auto last_char = dict->get_value(CommonNames::LastChar).get<int>();
    auto widths_array = MUST(dict->get_array(document, CommonNames::Widths));

    VERIFY(widths_array->size() == static_cast<size_t>(last_char - first_char + 1));

    HashMap<u16, u16> widths;
    for (size_t i = 0; i < widths_array->size(); i++)
        widths.set(first_char + i, widths_array->at(i).to_int());

    u16 missing_width = 0;
    auto descriptor = MUST(dict->get_dict(document, CommonNames::FontDescriptor));
    if (descriptor->contains(CommonNames::MissingWidth))
        missing_width = descriptor->get_value(CommonNames::MissingWidth).to_int();
    if (!descriptor->contains(CommonNames::FontFile))
        return Type1Font::Data { {}, to_unicode, encoding.release_nonnull(), move(widths), missing_width, true };

    auto font_file_stream = TRY(descriptor->get_stream(document, CommonNames::FontFile));
    auto font_file_dict = font_file_stream->dict();

    if (!font_file_dict->contains(CommonNames::Length1, CommonNames::Length2))
        return Error { Error::Type::Parse, "Embedded type 1 font is incomplete" };

    auto length1 = font_file_dict->get_value(CommonNames::Length1).get<int>();
    auto length2 = font_file_dict->get_value(CommonNames::Length2).get<int>();

    auto font_program = adopt_ref(*new PS1FontProgram());
    TRY(font_program->parse(font_file_stream->bytes(), length1, length2));
    encoding = font_program->encoding();

    return Type1Font::Data { font_program, to_unicode, encoding.release_nonnull(), move(widths), missing_width, false };
}

PDFErrorOr<NonnullRefPtr<Type1Font>> Type1Font::create(Document* document, NonnullRefPtr<DictObject> dict)
{
    auto data = TRY(Type1Font::parse_data(document, dict));
    return adopt_ref(*new Type1Font(data));
}

Type1Font::Type1Font(Data data)
    : m_data(move(data))
{
    m_is_standard_font = data.is_standard_font;
}

u32 Type1Font::char_code_to_code_point(u16 char_code) const
{
    if (m_data.to_unicode)
        TODO();

    auto descriptor = m_data.encoding->get_char_code_descriptor(char_code);
    return descriptor.code_point;
}

float Type1Font::get_char_width(u16 char_code, float) const
{
    u16 width;
    if (auto char_code_width = m_data.widths.get(char_code); char_code_width.has_value()) {
        width = char_code_width.value();
    } else {
        width = m_data.missing_width;
    }

    return static_cast<float>(width) / 1000.0f;
}

void Type1Font::draw_glyph(Gfx::Painter& painter, Gfx::IntPoint const& point, float width, u32 code_point, Color color)
{
    if (!m_data.font_program)
        return;

    RefPtr<Gfx::Bitmap> bitmap;

    auto maybe_bitmap = m_glyph_cache.get(code_point);
    if (maybe_bitmap.has_value()) {
        bitmap = maybe_bitmap.value();
    } else {
        bitmap = m_data.font_program->rasterize_glyph(code_point, width);
        m_glyph_cache.set(code_point, bitmap);
    }

    auto translation = m_data.font_program->glyph_translation(code_point, width);
    painter.blit_filtered(point.translated(translation.to_rounded<int>()), *bitmap, bitmap->rect(), [color](Color pixel) -> Color {
        return pixel.multiply(color);
    });
}
}
