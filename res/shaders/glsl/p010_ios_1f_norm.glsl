precision highp float;
uniform sampler2D u_t0;
uniform sampler2D u_t1;
uniform mat4 u_colormtx;
uniform vec4 u_color;
varying vec2 f_tex0;

float p010(vec2 bytes) {
  return (bytes.x * 255.0 + bytes.y * 65280.0) / 65472.0;
}
void main() {
  vec4 yb = texture2D(u_t0, f_tex0);
  vec4 uvb = texture2D(u_t1, f_tex0);
  float y = p010(vec2(yb.r, yb.a));
  vec2 uv = vec2(p010(uvb.rg), p010(uvb.ba));
  vec3 rgb = vec3(u_colormtx * vec4(y, uv, 1.0));
  gl_FragColor = vec4(rgb, u_color.a);
}
