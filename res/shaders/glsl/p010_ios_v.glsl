attribute vec4 a_position;
attribute vec4 a_texcoord;
uniform mat4 u_modelview;

const mat4 projection = mat4(2.414213,0.0,0.0,0.0,
                             0.0,2.414213,0.0,0.0,
                             0.0,0.0,1.033898,-1.0,
                             0.0,0.0,2.033898,0.0);
varying vec2 f_tex0;
void main() {
  gl_Position = projection * u_modelview * vec4(a_position.xyz, 1.0);
  f_tex0 = a_texcoord.xy;
}
