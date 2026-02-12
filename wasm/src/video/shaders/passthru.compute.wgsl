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

fn load(tex: texture_2d<f32>, x: u32, y: u32) -> f32 {

  // https://www.w3.org/TR/WGSL/#textureload
  // If an out of bounds access occurs, the built-in function returns one of:
  // - The data for some texel within bounds of the texture
  // - A vector (0,0,0,0) or (0,0,0,1) of the appropriate type for non-depth textures
  // - 0.0 for depth textures
  // とあるので、実装によって結果が違うかも・・・
  return textureLoad(tex, vec2<u32>(x, y), 0)[0];
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
  let u = (load(currentU, col, row) - 128.0 / 255.0) * (128.0 / (128.0 - 16.0));
  let v = (load(currentV, col, row) - 128.0 / 255.0) * (128.0 / (128.0 - 16.0));
  let y00 = (load(currentY, 2 * col + 0, 2 * row + 0) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y01 = (load(currentY, 2 * col + 0, 2 * row + 1) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y10 = (load(currentY, 2 * col + 1, 2 * row + 0) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
  let y11 = (load(currentY, 2 * col + 1, 2 * row + 1) - 16.0 / 255.0) * (255.0 / (235.0 - 16.0));
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
