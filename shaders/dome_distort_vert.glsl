
uniform vec3 mesh_focus; // focal point of dome
uniform float mesh_radius;
uniform sampler2D fbo_texture;
varying vec2 f_texcoord;
 
void main(void) {
    float mesh_dist = distance(vec2(gl_Vertex), vec2(mesh_focus));
    float z_delta = sqrt(pow(mesh_radius,2) - pow(mesh_dist,2));
    vec4 vert = gl_Vertex;
    vert.z = mesh_focus.z - z_delta;
    gl_Position = gl_ModelViewProjectionMatrix * vert;
    f_texcoord = gl_MultiTexCoord0.st;
}

