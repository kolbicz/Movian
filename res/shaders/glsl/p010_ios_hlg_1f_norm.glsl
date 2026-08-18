precision highp float;
uniform sampler2D u_t0;
uniform sampler2D u_t1;
uniform vec4 u_color;
varying vec2 f_tex0;
float p010(vec2 b) { return (b.x*255.0+b.y*65280.0)/65472.0; }
vec3 yuv(float y, vec2 uv) {
  y=(y-64.0/1023.0)*(1023.0/876.0);
  uv=(uv-vec2(512.0/1023.0))*(1023.0/896.0);
  return vec3(y+1.4746*uv.y,y-0.164553*uv.x-0.571353*uv.y,y+1.8814*uv.x);
}
vec3 hlg(vec3 e) {
  float a=0.17883277,b=0.28466892,c=0.55991073;
  return mix(e*e/3.0,(exp((e-c)/a)+b)/12.0,step(vec3(0.5),e));
}
vec3 gamut(vec3 c) { return vec3(1.6605*c.r-0.5876*c.g-0.0728*c.b,-0.1246*c.r+1.1329*c.g-0.0083*c.b,-0.0182*c.r-0.1006*c.g+1.1187*c.b); }
vec3 tone(vec3 x) {
  float l=dot(x,vec3(0.2126,0.7152,0.0722)), knee=0.75;
  if(l<=knee) return clamp(x,0.0,1.0);
  float t=clamp((l-knee)/(10.0-knee),0.0,1.0);
  float m=knee+(1.0-knee)*(1.0-exp(-3.0*t))/(1.0-exp(-3.0));
  return clamp(x*(m/max(l,0.0001)),0.0,1.0);
}
vec3 srgb(vec3 x) { return mix(12.92*x,1.055*pow(max(x,vec3(0.0)),vec3(1.0/2.4))-0.055,step(vec3(0.0031308),x)); }
void main() {
  vec4 yb=texture2D(u_t0,f_tex0), ub=texture2D(u_t1,f_tex0);
  vec3 c=max(yuv(p010(vec2(yb.r,yb.a)),vec2(p010(ub.rg),p010(ub.ba))),vec3(0.0));
  gl_FragColor=vec4(srgb(tone(max(gamut(pow(hlg(c),vec3(1.2))*4.0),vec3(0.0)))),u_color.a);
}
