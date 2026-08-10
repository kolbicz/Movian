#extension GL_ARB_texture_rectangle : enable

uniform sampler2DRect u_t0;
uniform sampler2DRect u_t1;
uniform vec4 u_color;
uniform float u_edr_headroom;
varying vec2 f_tex0;

vec3 bt2020_ycbcr_to_rgb(float y, vec2 uv)
{
  y = (y - 64.0 / 1023.0) * (1023.0 / 876.0);
  uv = (uv - vec2(512.0 / 1023.0)) * (1023.0 / 896.0);
  return vec3(y + 1.4746 * uv.y,
              y - 0.164553 * uv.x - 0.571353 * uv.y,
              y + 1.8814 * uv.x);
}

vec3 hlg_inverse_oetf(vec3 e)
{
  const float a = 0.17883277;
  const float b = 0.28466892;
  const float c = 0.55991073;
  vec3 lo = e * e / 3.0;
  vec3 hi = (exp((e - c) / a) + b) / 12.0;
  return mix(lo, hi, step(vec3(0.5), e));
}

vec3 bt2020_to_bt709(vec3 c)
{
  return vec3( 1.6605*c.r - 0.5876*c.g - 0.0728*c.b,
              -0.1246*c.r + 1.1329*c.g - 0.0083*c.b,
              -0.0182*c.r - 0.1006*c.g + 1.1187*c.b);
}

vec3 linear_to_srgb(vec3 x)
{
  vec3 lo = 12.92 * x;
  vec3 hi = 1.055 * pow(max(x, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), x));
}

void main()
{
  float y = texture2DRect(u_t0, f_tex0).r;
  vec2 uv = texture2DRect(u_t1, f_tex0 * 0.5).rg;
  vec3 hlg = max(bt2020_ycbcr_to_rgb(y, uv), vec3(0.0));
  vec3 linear2020 = pow(hlg_inverse_oetf(hlg), vec3(1.2)) * 4.0;
  vec3 linear709 = max(bt2020_to_bt709(linear2020), vec3(0.0));
  vec3 edr = linear_to_srgb(linear709);
  gl_FragColor = vec4(min(edr, vec3(u_edr_headroom)), u_color.a);
}
