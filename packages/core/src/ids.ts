/**
 * id 生成。刻意放在 core 之外的单文件里，方便在测试中注入确定性实现。
 *
 * 形态：`<prefix>_<22 位 base32 时间有序串>`。
 * 时间有序的好处是：日志里同一批生成的节点天然按创建顺序排列，肉眼可查。
 */

const ALPHABET = '0123456789abcdefghjkmnpqrstvwxyz';

/** 单调递增计数器，保证同一毫秒内生成的 id 不重复。 */
let counter = 0;
let lastTime = 0;

export function createId(prefix: string, now: number = Date.now()): string {
  if (now <= lastTime) {
    counter = (counter + 1) % 0x1000;
  } else {
    lastTime = now;
    counter = 0;
  }

  let time = now;
  let timePart = '';
  for (let index = 0; index < 10; index += 1) {
    timePart = ALPHABET[time % 32] + timePart;
    time = Math.floor(time / 32);
  }

  let randomPart = '';
  const bytes = new Uint8Array(8);
  globalThis.crypto.getRandomValues(bytes);
  for (const byte of bytes) randomPart += ALPHABET[byte % 32];

  const counterPart = ALPHABET[counter % 32];
  return `${prefix}_${timePart}${counterPart}${randomPart}`;
}

/** 测试用：可复现的 id 生成器。 */
export function createSequentialIdFactory(prefix: string): () => string {
  let index = 0;
  return () => {
    index += 1;
    return `${prefix}_${String(index).padStart(4, '0')}`;
  };
}
