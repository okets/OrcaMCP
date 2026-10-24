#version 140

uniform bool ban_light;
uniform vec4 uniform_color;
uniform float emission_factor;
// A fragment with dot(world_pos, clipping_plane) < 0 is discarded; (0, 0, 0, 1) clips nothing.
uniform vec4 clipping_plane;

// x = tainted, y = specular;
in vec2 intensity;
//varying float drop;
in vec4 world_pos;

out vec4 out_color;

void main()
{
    if (world_pos.z < 0.0)
        discard;
    if (dot(world_pos, clipping_plane) < 0.0)
        discard;
    if(ban_light){
       out_color = uniform_color;
    } else{
       out_color = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
    }
}
