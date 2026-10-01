// A Box2D/NavierStokes-shaped double kernel: several live doubles, a loop-carried accumulator.
function step(ax, ay, bx, by, n) {
  let vx = 0.5, vy = -0.25, px = ax, py = ay, e = 0;
  for (let i = 0; i < n; i++) {
    const dx = bx - px, dy = by - py;
    const d2 = dx * dx + dy * dy + 1e-9;
    const inv = 1.0 / Math.sqrt(d2);
    const f = inv * inv * inv;
    vx += dx * f * 0.001; vy += dy * f * 0.001;
    px += vx * 0.01; py += vy * 0.01;
    e += 0.5 * (vx * vx + vy * vy);
  }
  return e + px + py;
}
let s = 0;
for (let k = 0; k < 200; k++) s += step(k * 0.1, 1.0, 3.0, -2.0, 20000);
console.log(s);
