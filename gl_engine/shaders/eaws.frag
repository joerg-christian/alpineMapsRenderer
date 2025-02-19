/*****************************************************************************
* AlpineMaps.org
* Copyright (C) 2022 Adam Celarek
* Copyright (C) 2023 Gerald Kimmersdorfer
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "shared_config.glsl"
#include "camera_config.glsl"
#include "encoder.glsl"
#include "tile_id.glsl"
#include "eaws.glsl"

uniform highp usampler2DArray ortho_sampler;
uniform mediump usampler2DArray height_tex_sampler;  //uniform mediump usampler2DArray height_tex_sampler;
uniform highp usampler2D height_tex_index_sampler;
uniform highp usampler2D height_tex_tile_id_sampler;
uniform highp usampler2D ortho_map_index_sampler;
uniform highp usampler2D ortho_map_tile_id_sampler;

layout (location = 0) out lowp vec3 texout_albedo;
layout (location = 1) out highp vec4 texout_position;
layout (location = 2) out highp uvec2 texout_normal;
layout (location = 3) out lowp vec4 texout_depth;
layout (location = 4) out lowp vec3 texout_eaws;

flat in highp uvec3 var_tile_id;
in highp vec2 var_uv;
in highp vec3 var_pos_cws;
in highp vec3 var_normal;
flat in highp int var_height_texture_layer;
#if CURTAIN_DEBUG_MODE > 0
in lowp float is_curtain;
#endif
flat in lowp vec3 vertex_color;

highp float calculate_falloff(highp float dist, highp float from, highp float to) {
    return clamp(1.0 - (dist - from) / (to - from), 0.0, 1.0);
}

highp vec3 normal_by_fragment_position_interpolation() {
    highp vec3 dFdxPos = dFdx(var_pos_cws);
    highp vec3 dFdyPos = dFdy(var_pos_cws);
    return normalize(cross(dFdxPos, dFdyPos));
}

lowp ivec2 to_dict_pixel(mediump uint hash) {
    return ivec2(int(hash & 255u), int(hash >> 8u));
}

bool find_tile(inout highp uvec3 tile_id, out lowp ivec2 dict_px, inout highp vec2 uv) {
    uvec2 missing_packed_tile_id = uvec2((-1u) & 65535u, (-1u) & 65535u);
    uint iter = 0u;
    do {
        mediump uint hash = hash_tile_id(tile_id);
        highp uvec2 wanted_packed_tile_id = pack_tile_id(tile_id);
        highp uvec2 found_packed_tile_id = texelFetch(ortho_map_tile_id_sampler, to_dict_pixel(hash), 0).xy;
        while(found_packed_tile_id != wanted_packed_tile_id && found_packed_tile_id != missing_packed_tile_id) {
            hash++;
            found_packed_tile_id = texelFetch(ortho_map_tile_id_sampler, to_dict_pixel(hash), 0).xy;
            if (iter++ > 50u) {
                break;
            }
        }
        if (found_packed_tile_id == wanted_packed_tile_id) {
            dict_px = to_dict_pixel(hash);
            tile_id = unpack_tile_id(wanted_packed_tile_id);
            return true;
        }
    }
    while (decrease_zoom_level_by_one(tile_id, uv));
    return false;
}

void main() {
#if CURTAIN_DEBUG_MODE == 2
    if (is_curtain == 0.0) {
        discard;
    }
#endif
    highp uvec3 tile_id = var_tile_id;
    highp vec2 uv = var_uv;

    lowp ivec2 dict_px;
    if (find_tile(tile_id, dict_px, uv)) {
        // texout_albedo = vec3(0.0, float(tile_id.z) / 20.0, 0.0);
        // texout_albedo = vec3(float(dict_px.x) / 255.0, float(dict_px.y) / 255.0, 0.0);
        // texout_albedo = vec3(uv.x, uv.y, 0.0);
        uint texture_layer = texelFetch(ortho_map_index_sampler, dict_px, 0).x;
        // texout_albedo = vec3(0.0, float(texture_layer_f) / 10.0, 0.0);
        int u = int(uv.x*255);
        int v = int(uv.y*255);
        highp uint eawsRegionId = texelFetch(ortho_sampler, ivec3(u,v,texture_layer),0).r;
        ivec4 report = eaws.reports[eawsRegionId];
        lowp vec3 fragColor;

        float frag_height = 0.125f * float(texture(height_tex_sampler, vec3(var_uv, var_height_texture_layer)).r);
        if(report.x == 1) // report.x = 0 means no report available .x=1 means report available
        {
            // get ratings for eaws refion of current fragment
            int bound = report.y;      // bound dividing moutain in Hi region and low region
            int ratingHi = report.a;   // rating should be value in {0,1,2,3,4}
            int ratingLo = report.z;   // rating should be value in {0,1,2,3,4}
            int rating = ratingLo;

            // color fragment according to danger level
            float margin = 25.f;           // margin within which colorblending between hi and lo happens
            if(frag_height > float(bound) + margin )
                fragColor =  color_from_eaws_danger_rating(ratingHi);
            else if (frag_height < float(bound) - margin)
                fragColor =  color_from_eaws_danger_rating(ratingLo);
            else
            {
                // around border: blend colors between upper and lower danger rating
                float a = (frag_height - (float(bound) - margin)) / (2*margin); // This is a value between 0 and 1
                fragColor = mix(color_from_eaws_danger_rating(ratingLo), color_from_eaws_danger_rating(ratingHi), a);
            }
        }
        else
        {
            fragColor = vec3(0,0,0);
        }
        texout_eaws = fragColor;
    }
    else {
        texout_eaws = vec3(1.0, 0.0, 0.5);
    }
}
