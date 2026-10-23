/**
 * 「讲讲它」的展示层：把模型回来的那段话、以及各种失败，折成节点图上
 * 那块小卡片能直接画的三行字。
 *
 * 为什么文案放在宿主这一侧：原生的节点图只会画字，不会写文案。失败要写成
 * 「人看完知道下一步干什么」的一句话，这条规则还必须能被单测按住——
 * 所以放在这里当纯函数，原生拿到什么就画什么。
 */

/** 卡片上要画的东西。phase 只有两种：讲成了、没讲成。 */
export interface LessonView {
  phase: 'done' | 'error';
  title: string;
  text: string;
  hint: string;
  /** true = 宿主根本没有这个接口（版本太旧）：节点图上那颗按钮该置灰，别让人一直点。 */
  unsupported: boolean;
}

/** 讲解正文的上限：卡片只有一行正文，再多也是被截断，不如在这里截干净。 */
export const LESSON_TEXT_MAX = 120;
/** 失败原因的上限：模型/宿主的报错经常带着一整段栈，卡片上只留一句能记住的。 */
export const LESSON_REASON_MAX = 60;

function truncate(text: string, max: number): string {
  const chars = Array.from(text);
  if (chars.length <= max) return text;
  return `${chars.slice(0, Math.max(1, max - 1)).join('').trimEnd()}…`;
}

/**
 * 把一段给人看的文字压成一行。
 *
 * 模型爱用 markdown：强调号、标题号、代码块、链接。节点图上没有富文本，
 * 原样画出来就是一堆记号，所以先剥壳再折行。折叠空白是为了让
 * 「第一句。第二句。」这样的讲解在卡片里读起来是一段，而不是断成几截。
 */
export function lessonText(raw: string, max: number = LESSON_TEXT_MAX): string {
  const flat = raw
    .replace(/```[\s\S]*?```/g, ' ') // 代码块整块去掉：卡片放不下，讲了也看不见
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1') // 链接只留字
    .replace(/[*_`#>]+/g, '') // 强调、标题、引用符号：直接抽掉，别在中文词之间留空格
    .replace(/^\s*[-•·]\s+/gm, '') // 列表符号
    .replace(/\s+/g, ' ')
    .trim();
  return truncate(flat, max);
}

/** 讲成了：正文归一化，副行优先放「试一下」，没有就提示怎么关掉。 */
export function lessonSuccess(text: string, alt?: string): LessonView {
  const body = lessonText(text);
  const followUp = typeof alt === 'string' ? lessonText(alt, 40) : '';
  return {
    phase: 'done',
    title: '讲讲它',
    text: body === '' ? '模型这次没给正文（回的是空）。' : body,
    hint: followUp === '' ? '按 Esc 关掉' : `试一下：${followUp}`,
    unsupported: false,
  };
}

/**
 * 没讲成。
 *
 * 每一档都要回答「我现在能做什么」：换一颗、等一等、去配模型、还是去重启。
 * 只说「失败」等于把人卡在原地——这条入口本来就是给不懂的人用的。
 */
export function lessonFailure(status: number | null, reason: string): LessonView {
  const why = truncate(lessonText(reason, LESSON_REASON_MAX), LESSON_REASON_MAX);
  const base = { phase: 'error' as const, unsupported: false };
  switch (status) {
    case null:
    case 0:
      return {
        ...base,
        title: '没连上宿主',
        text: '宿主没在跑，或者刚刚重启过。',
        hint: '把 dsh 重新拉起（--profile dshwork-dev），再点一次',
      };
    case 405:
    case 501:
      return {
        ...base,
        unsupported: true,
        title: '这个宿主还不认识讲解',
        text: '跑着的宿主比节点图旧，接口还没长出来。',
        hint: '拉最新代码重启宿主；按钮恢复前别重复点',
      };
    case 404:
      return {
        ...base,
        title: '这颗节点不在了',
        text: why === '' ? '宿主那边查不到它。' : why,
        hint: '按 ↑↓ 换一颗；下次打开节点图会拿到最新的树',
      };
    case 409:
      return {
        ...base,
        title: '现在没法讲',
        text: why === '' ? '这一步正卡在别的动作上。' : why,
        hint: '等它做完再点一次',
      };
    case 400:
      return {
        ...base,
        title: '请求没看懂',
        text: why === '' ? '宿主没解析出要讲哪一颗。' : why,
        hint: 'Esc 关掉、重开节点图再试',
      };
    case 503:
      return {
        ...base,
        title: '宿主没接模型',
        text: why === '' ? '这台宿主没有可用的 llm 服务。' : why,
        hint: '在 DSH 里配一个模型，或者换个 provider 再点',
      };
    default:
      return {
        ...base,
        title: '讲的时候出错了',
        text: why === '' ? `宿主回了 ${status ?? '未知状态'}` : why,
        hint: '看一眼 dsh-work 的日志，再试一次',
      };
  }
}
