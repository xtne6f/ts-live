R"(
@group(0) @binding(0) var mySampler : sampler;
@group(0) @binding(1) var outputFrame :  texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var currentY : texture_2d<f32>;
@group(0) @binding(3) var currentU : texture_2d<f32>;
@group(0) @binding(4) var currentV : texture_2d<f32>;
@group(0) @binding(5) var prevY : texture_2d<f32>;
@group(0) @binding(6) var prevU : texture_2d<f32>;
@group(0) @binding(7) var prevV : texture_2d<f32>;
@group(0) @binding(8) var nextY : texture_2d<f32>;
@group(0) @binding(9) var nextU : texture_2d<f32>;
@group(0) @binding(10) var nextV : texture_2d<f32>;

const coef_lf = vec2<f32>(4309.0, 213.0) / (1 << 13);
const coef_hf = vec3<f32>(5570.0, 3801.0, 1016.0) / (1 << 15);
const coef_sp = vec2<f32>(5077.0, 981.0) / (1 << 13);

fn load(tex: texture_2d<f32>, x: u32, y: u32) -> f32 {

  // https://www.w3.org/TR/WGSL/#textureload
  // If an out of bounds access occurs, the built-in function returns one of:
  // - The data for some texel within bounds of the texture
  // - A vector (0,0,0,0) or (0,0,0,1) of the appropriate type for non-depth textures
  // - 0.0 for depth textures
  // とあるので、実装によって結果が違うかも・・・
  return textureLoad(tex, vec2<u32>(x, y), 0)[0];
}

fn avg(a: f32, b: f32) -> f32 {
  return (a + b) * 0.5;
}

fn max3(a: f32, b: f32, c: f32) -> f32 {
  return max(max(a, b), c);
}

fn min3(a: f32, b: f32, c: f32) -> f32 {
  return (min(min(a, b), c));
}

fn filter_(cur_prefs3: f32, cur_prefs: f32, cur_mrefs: f32, cur_mrefs3: f32,
           prev2_prefs4: f32, prev2_prefs2: f32, prev2_0: f32, prev2_mrefs2: f32, prev2_mrefs4: f32,
           prev_prefs: f32, prev_mrefs: f32,
           // next_prefs: f32, next_mrefs: f32,
           next2_prefs4: f32, next2_prefs2: f32, next2_0: f32, next2_mrefs2: f32, next2_mrefs4: f32) -> f32 {

  // FFmpeg/libavfilter/vf_bwdif_cuda.cu (clip_max == 1.0 の場合) を参考にした。

  let c = cur_mrefs;
  let d = avg(prev2_0, next2_0);
  let e = cur_prefs;

  let temporal_diff0 = abs(prev2_0 - next2_0);
  let temporal_diff1 = avg(abs(prev_mrefs - c), abs(prev_prefs - e));
  // let temporal_diff2 = avg(abs(next_mrefs - c), abs(next_prefs - e));

  // var diff = max3(temporal_diff0 * 0.5, temporal_diff1, temporal_diff2);
  var diff = max(temporal_diff0 * 0.5, temporal_diff1);
  if (diff == 0.0) {
    return d;
  }

  let b = avg(prev2_mrefs2, next2_mrefs2) - c;
  let f = avg(prev2_prefs2, next2_prefs2) - e;
  let dc = d - c;
  let de = d - e;
  let mmax = max3(de, dc, min(b, f));
  let mmin = min3(de, dc, max(b, f));
  diff = max3(diff, mmin, -mmax);

  var interpol: f32;
  if (abs(c - e) > temporal_diff0) {
    interpol = coef_hf[0] * (prev2_0 + next2_0) -
               coef_hf[1] * (prev2_mrefs2 + next2_mrefs2 + prev2_prefs2 + next2_prefs2) +
               coef_hf[2] * (prev2_mrefs4 + next2_mrefs4 + prev2_prefs4 + next2_mrefs4) +
               coef_lf[0] * (c + e) - coef_lf[1] * (cur_mrefs3 + cur_prefs3);
  } else {
    interpol = coef_sp[0] * (c + e) - coef_sp[1] * (cur_mrefs3 + cur_prefs3);
  }
  return saturate(clamp(interpol, d - diff, d + diff));
}

fn bwdif(cur: texture_2d<f32>, prev: texture_2d<f32>, next: texture_2d<f32>, x: u32, y: u32, h: u32) -> f32 {
  if (y % 2 == 0) {
    return load(cur, x, y);
  }

  // 画像端はミラーリング
  let my2 = select(y - 2, y + 2, y < 2);
  let my3 = select(y - 3, y + 3, y < 3);
  let my4 = select(y - 4, y + 4, y < 4);
  let py1 = select(y + 1, y - 1, y >= h - 1);
  let py2 = select(y + 2, y - 2, y >= h - 2);
  let py3 = select(y + 3, y - 3, y >= h - 3);
  let py4 = select(y + 4, y - 4, y >= h - 4);

  // is_second_field == false の場合

  return filter_(
    load(cur, x, py3),
    load(cur, x, py1),
    load(cur, x, y - 1),
    load(cur, x, my3),

    load(prev, x, py4),
    load(prev, x, py2),
    load(prev, x, y),
    load(prev, x, my2),
    load(prev, x, my4),

    load(prev, x, py1),
    load(prev, x, y - 1),
    // load(cur, x, py1),
    // load(cur, x, y - 1),

    load(next, x, py4),
    load(next, x, py2),
    load(next, x, y),
    load(next, x, my2),
    load(next, x, my4));
}

fn yuv2rgba(y: f32, u: f32, v: f32) -> vec4<f32> {
  return vec4<f32>(
    saturate(y + 1.5748 * v),
    saturate(y - 0.1873 * u - 0.4681 * v),
    saturate(y + 1.8556 * u),
    1.0);
}

@compute
@workgroup_size(16, 4, 1)
fn main(
  @builtin(global_invocation_id) coord3: vec3<u32>
) {
  let col = coord3[0];
  let row = coord3[1];
  let u = (bwdif(currentU, prevU, nextU, col, row, textureDimensions(currentU)[1]) - 128.0 / 255.0) * (128.0 / (128.0 - 16.0));
  let v = (bwdif(currentV, prevV, nextV, col, row, textureDimensions(currentV)[1]) - 128.0 / 255.0) * (128.0 / (128.0 - 16.0));
  let h = textureDimensions(currentY)[1];
  let y00 = (bwdif(currentY, prevY, nextY, 2 * col + 0, 2 * row + 0, h) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y01 = (bwdif(currentY, prevY, nextY, 2 * col + 0, 2 * row + 1, h) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y10 = (bwdif(currentY, prevY, nextY, 2 * col + 1, 2 * row + 0, h) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y11 = (bwdif(currentY, prevY, nextY, 2 * col + 1, 2 * row + 1, h) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let rgba00 = yuv2rgba(y00, u, v);
  let rgba01 = yuv2rgba(y01, u, v);
  let rgba10 = yuv2rgba(y10, u, v);
  let rgba11 = yuv2rgba(y11, u, v);
  textureStore(outputFrame, vec2<u32>(2 * col + 0, 2 * row + 0), rgba00);
  textureStore(outputFrame, vec2<u32>(2 * col + 0, 2 * row + 1), rgba01);
  textureStore(outputFrame, vec2<u32>(2 * col + 1, 2 * row + 0), rgba10);
  textureStore(outputFrame, vec2<u32>(2 * col + 1, 2 * row + 1), rgba11);
}
)"
