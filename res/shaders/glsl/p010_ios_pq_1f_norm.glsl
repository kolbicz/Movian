precision highp float;
uniform sampler2D u_t0;
uniform sampler2D u_t1;
uniform vec4 u_color;
uniform float u_hdr_peak_luminance;
varying vec2 f_tex0;
float p010(vec2 b) { return (b.x*255.0+b.y*65280.0)/65472.0; }
vec3 yuv(float y, vec2 uv) {
  y=(y-64.0/1023.0)*(1023.0/876.0);
  uv=(uv-vec2(512.0/1023.0))*(1023.0/896.0);
  return vec3(y+1.4746*uv.y,y-0.164553*uv.x-0.571353*uv.y,y+1.8814*uv.x);
}
vec3 pq(vec3 e) {
  float m1=0.1593017578125,m2=78.84375,c1=0.8359375,c2=18.8515625,c3=18.6875;
  vec3 p=pow(max(e,vec3(0.0)),vec3(1.0/m2));
  return pow(max(p-c1,vec3(0.0))/max(c2-c3*p,vec3(0.00001)),vec3(1.0/m1));
}
vec3 gamut(vec3 c) { return vec3(1.6605*c.r-0.5876*c.g-0.0728*c.b,-0.1246*c.r+1.1329*c.g-0.0083*c.b,-0.0182*c.r-0.1006*c.g+1.1187*c.b); }
vec3 tone(vec3 x) { return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }
vec3 tonehdr(vec3 x) { float sourcePeak=max(1.0,u_hdr_peak_luminance/100.0); return tone(x*(10.0/sourcePeak)); }
vec3 srgb(vec3 x) { return mix(12.92*x,1.055*pow(max(x,vec3(0.0)),vec3(1.0/2.4))-0.055,step(vec3(0.0031308),x)); }
void main() {
  vec4 yb=texture2D(u_t0,f_tex0), ub=texture2D(u_t1,f_tex0);
  vec3 c=max(yuv(p010(vec2(yb.r,yb.a)),vec2(p010(ub.rg),p010(ub.ba))),vec3(0.0));
  gl_FragColor=vec4(srgb(tonehdr(max(gamut(pq(c)*100.0),vec3(0.0)))),u_color.a);
}
