/**
 * 事件流：把 journal 里的一条意图翻译成「人一眼能读的一行」。
 *
 * 为什么要有它：任务文档只有「当前/待办」，人能看见状态却看不见**过程**——
 * AI 刚接了哪个节点、哪颗被退回过、刚刚拆了几件事，全都只存在日志里。
 * 这层只做翻译，不做存储：原始事实仍然只有 journal 一份。
 */

/** journal.ndjson 里一行（见 store.ts 的提交路径）。 */
export interface JournalLine {
  seq: number;
  /** ISO 时间串，写日志那一刻。 */
  at: string;
  by: 'human' | 'ai' | 'system';
  intent: Record<string, unknown> & { type?: string };
}

/** 时间戳 → 本地 HH:mm。日志里既有毫秒数也有老版本写的 ISO 串。 */
export function clock(at: number | string): string {
  const ms = typeof at === 'number' ? at : Date.parse(at);
  if (!Number.isFinite(ms)) return '';
  const d = new Date(ms);
  const hh = String(d.getHours()).padStart(2, '0');
  const mm = String(d.getMinutes()).padStart(2, '0');
  return hh + ':' + mm;
}
/** 面板/节点图上要显示的一行。 */
export interface FeedItem {
  seq: number;
  at: string;
  /** 动词短标签，两三个字，给芯片用。 */
  verb: string;
  /** 正文：多数情况下是节点标题，plan.set 则是体量。 */
  text: string;
  /** 结局是否顺：退回、失败为 false，其余 true。 */
  ok: boolean;
}

/**
 * 把意图翻成一行。
 *
 * `titleOf` 由调用方注入（当前文档的 id→标题），这样本文件不依赖文档结构，
 * 可以纯函数单测——日志里的意图只带 id，标题是会变的，翻译时再取最新值。
 */
export function describeEntry(line: JournalLine, titleOf: (id: string) => string): FeedItem {
  const intent = line.intent ?? {};
  const type = typeof intent.type === 'string' ? intent.type : '';
  const id = typeof intent.nodeId === 'string' ? intent.nodeId : '';
  const title = id === '' ? '' : titleOf(id);

  const item = (verb: string, text: string, ok = true): FeedItem => ({
    seq: line.seq,
    at: line.at,
    verb,
    text,
    ok,
  });

  switch (type) {
    case 'plan.set': {
      const nodes = Array.isArray(intent.nodes) ? intent.nodes.length : 0;
      const title_ = typeof intent.title === 'string' && intent.title !== '' ? `「${intent.title}」` : '';
      return item('拆解', `生成 ${nodes} 件事${title_}`);
    }
    case 'node.advance':
      return item('推进', '完成当前并前进');
    case 'node.complete':
      return item('完成', title || '一件事');
    case 'node.focus':
      return item('切到', title || '一件事');
    case 'handoff.offer':
      return item('交给 AI', title || '一件事');
    case 'handoff.start':
      return item('接手', title || '一件事');
    case 'handoff.submit':
      return item('交付', title || '一件事');
    case 'handoff.accept':
      return item('验收', title || '一件事');
    case 'handoff.return':
      return item('退回', title || '一件事', false);
    default:
      // 未知意图照常显示，别让日志里的新东西在界面上静默消失
      return item('·', type === '' ? '（无类型）' : type);
  }
}

/** 给标题提供一个兜底：拿不到就显示 id 的尾巴，总比空着强。 */
export function titleLookup(nodes: Record<string, { title?: string }>): (id: string) => string {
  return (id) => {
    const node = nodes[id];
    if (node && typeof node.title === 'string' && node.title !== '') return node.title;
    return id === '' ? '' : `#${id.slice(-6)}`;
  };
}
