#extension GL_ARB_texture_rectangle : enable

uniform sampler2DRect u_t0;
uniform sampler2DRect u_t1;
uniform vec4 u_color;
varying vec2 f_tex0;

vec3 bt2020_ycbcr_to_rgb(float y, vec2 uv)
{
  y = (y - 64.0 / 1023.0) * (1023.0 / 876.0);
  uv = (uv - vec2(512.0 / 1023.0)) * (1023.0 / 896.0);
  return vec3(y + 1.4746 * uv.y,
              y - 0.164553 * uv.x - 0.571353 * uv.y,
              y + 1.8814 * uv.x);
}

vec3 pq_eotf(vec3 e)
{
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  vec3 p = pow(max(e, vec3(0.0)), vec3(1.0 / m2));
  return pow(max(p - c1, vec3(0.0)) / max(c2 - c3 * p, vec3(0.00001)),
             vec3(1.0 / m1));
}

vec3 bt2020_to_bt709(vec3 c)
{
  return vec3( 1.6605*c.r - 0.5876*c.g - 0.0728*c.b,
              -0.1246*c.r + 1.1329*c.g - 0.0083*c.b,
              -0.0182*c.r - 0.1006*c.g + 1.1187*c.b);
}

vec3 aces_sdr(vec3 x)
{
  return clamp((x * (2.51*x + 0.03)) /
               (x * (2.43*x + 0.59) + 0.14), 0.0, 1.0);
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
  vec3 pq = max(bt2020_ycbcr_to_rgb(y, uv), vec3(0.0));
  vec3 linear2020 = pq_eotf(pq) * 100.0; // 10,000-nit PQ to 100-nit SDR units
  vec3 linear709 = max(bt2020_to_bt709(linear2020), vec3(0.0));
  gl_FragColor = vec4(linear_to_srgb(aces_sdr(linear709)), u_color.a);
}
