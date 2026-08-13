#extension GL_ARB_texture_rectangle : enable

uniform sampler2DRect u_t0;
uniform vec4 u_color;
varying vec2 f_tex0;

void main()
{
  vec4 c = texture2DRect(u_t0, f_tex0);
  gl_FragColor = vec4(c.rgb, c.a * u_color.a);
}
