#include "hv_render.h"

const char *hv_render_read(const uint8_t *buf, const hv_presentation *p,
                           size_t layer, size_t output_components,
                           hv_render *render, size_t *at) {
    hv_render value = {0};
    hv_presentation_headers headers;
    hv_registration_entry registration;
    hv_component_mapping mapping = {0};
    size_t selected[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX}, pos = 0;
    const char *error;
    if (layer >= p->layers) {
        error = "presentation layer index out of range";
        goto fail;
    }
    const hv_layer *own = &p->layer[layer];
    pos = own->header.start;
    if (own->registration.count != 1) {
        error = "unsupported multiple-codestream composition";
        goto fail;
    }
    error = hv_registration_read(&own->registration, 0, &registration);
    if (error) goto fail;
    if (registration.alignment_x || registration.alignment_y ||
        registration.sampling_x != own->registration.denominator_x ||
        registration.sampling_y != own->registration.denominator_y) {
        error = "unsupported nonidentity registration";
        goto fail;
    }
    error = hv_presentation_read_headers(buf, p, layer, 0, &headers, &pos);
    if (error) goto fail;
    value.codestream = headers.codestream;
    if (headers.opacity.type) {
        pos = headers.opacity.start;
        error = "unsupported opacity";
        goto fail;
    }
    if (headers.pixel_format.type) {
        pos = headers.pixel_format.start;
        error = "unsupported pixel format";
        goto fail;
    }
    if (!output_components) {
        error = "no decoder output components";
        goto fail;
    }

    hv_boxes colours;
    hv_box box;
    int status, precedence = -129, have_colour = 0;
    hv_colour chosen = {0};
    size_t chosen_at = 0;
    hv_boxes_children(&colours, buf, &headers.colour);
    while ((status = hv_boxes_next(&colours, &box, &error, &pos)) == 1) {
        if (box.type != HV_BOX_COLR) continue;
        have_colour = 1;
        hv_colour colour;
        error = hv_colour_read(buf, &box, &colour, &pos);
        if (error) goto fail;
        if (colour.method != 1 || (colour.colour_space != 16 && colour.colour_space != 17))
            continue;
        int priority = p->is_jpx ? colour.precedence : 0;
        if (priority > precedence) {
            precedence = priority;
            chosen = colour;
            chosen_at = box.start;
            value.colour_space = colour.colour_space;
            value.channel_count = colour.colour_space == 16 ? 3 : 1;
        }
    }
    if (status < 0) goto fail;
    if (!value.channel_count) {
        pos = headers.colour.start;
        error = have_colour ? "unsupported colour conversion" : "missing colour description";
        goto fail;
    }

    if (chosen.size) {
        pos = chosen_at;
        error = "unexpected grayscale/RGB colour parameters";
        goto fail;
    }

    if (headers.mapping.type) {
        error = hv_component_mapping_open(buf, &headers.mapping, &mapping, &pos);
        if (error) goto fail;
    }
    size_t channels = headers.mapping.type ? mapping.count : output_components;
    if (headers.channels.type) {
        hv_channel_definition definition;
        error = hv_channel_definition_open(buf, &headers.channels, &definition, &pos);
        if (error) goto fail;
        for (size_t n = 0; n < definition.count; n++) {
            hv_channel_definition_entry entry;
            hv_channel_definition_read(&definition, n, &entry);
            /* Unassociated/unspecified entries have no rendering role. */
            if (entry.association == 65535 || entry.type == 65535) continue;
            pos = headers.channels.payload + 2 + n * 6;
            if (entry.type == 1 || entry.type == 2) {
                error = "unsupported opacity";
                goto fail;
            }
            if (entry.type != 0) {
                error = "unsupported channel role";
                goto fail;
            }
            if (entry.channel >= channels || entry.association > value.channel_count) {
                error = "colour channel reference out of range";
                goto fail;
            }
            size_t first = entry.association ? entry.association - 1 : 0;
            size_t end = entry.association ? first + 1 : value.channel_count;
            for (size_t c = first; c < end; c++) {
                if (selected[c] != SIZE_MAX) {
                    error = "duplicate colour channel association";
                    goto fail;
                }
                selected[c] = entry.channel;
            }
        }
    } else {
        for (size_t c = 0; c < value.channel_count; c++) selected[c] = c;
    }
    for (size_t c = 0; c < value.channel_count; c++) {
        if (selected[c] == SIZE_MAX || selected[c] >= channels) {
            pos = headers.channels.type ? headers.channels.start : headers.mapping.start;
            error = "incomplete colour channel definitions";
            goto fail;
        }
        hv_render_channel *channel = &value.channel[c];
        channel->component = selected[c];
        channel->palette_column = -1;
        if (headers.mapping.type) {
            hv_component_mapping_entry entry;
            hv_component_mapping_read(&mapping, selected[c], &entry);
            pos = headers.mapping.payload + selected[c] * 4;
            channel->component = entry.component;
            if (entry.type == 1) {
                if (!value.palette.entry_count) {
                    if (!headers.palette.type) {
                        error = "missing mapped palette";
                        goto fail;
                    }
                    error = hv_palette_open(buf, &headers.palette, &value.palette, &pos);
                    if (error) goto fail;
                }
                if (entry.column >= value.palette.column_count) {
                    error = "palette column out of range";
                    goto fail;
                }
                channel->palette_column = entry.column;
            } else if (entry.type != 0) {
                error = "unsupported component mapping method";
                goto fail;
            }
        }
        if (channel->component >= output_components) {
            error = "decoder output component out of range";
            goto fail;
        }
    }
    *render = value;
    return NULL;
fail:
    *at = pos;
    return error;
}
