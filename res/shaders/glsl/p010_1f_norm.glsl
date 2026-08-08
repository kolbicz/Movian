#extension GL_ARB_texture_rectangle : enable

uniform sampler2DRect u_t0;
uniform sampler2DRect u_t1;
uniform mat4 u_colormtx;
uniform vec4 u_color;

varying vec2 f_tex0;

void main()
{
  float y = texture2DRect(u_t0, f_tex0).r;
  vec4 uv = texture2DRect(u_t1, f_tex0 * 0.5);
  vec3 rgb = vec3(u_colormtx * vec4(y, uv.r, uv.g, 1.0));
  gl_FragColor = vec4(rgb, u_color.a);
}
