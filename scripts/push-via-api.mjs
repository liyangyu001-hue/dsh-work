// 推送通道：当 github.com:443 不通、api.github.com 通时，用它代替 git push。
// 走 GitHub Git Data API 逐提交重放，保住历史、作者与日期。
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

const OWNER = "liyangyu001-hue";
const REPO = "dsh-work";
const TOKEN = readFileSync(process.env.USERPROFILE + "/.dsh/dsh-work/gh-token", "utf8").trim();
const REMOTE_SHA = process.argv[2];

if (!REMOTE_SHA) {
  console.error("用法：node scripts/push-via-api.mjs <远端当前 sha>");
  process.exit(1);
}
const CWD = process.cwd();

// 不用 keep-alive：Node 24 在退出阶段回收 undici 连接池会触发 libuv 断言，
// 表现为"活干完了但进程崩"，而 Connection: close 让每个请求自成一体。
const api = async (path, init = {}) => {
  const res = await fetch(`https://api.github.com${path}`, {
    ...init,
    headers: {
      Authorization: `Bearer ${TOKEN}`,
      Accept: "application/vnd.github+json",
      "User-Agent": "dsh-work-push",
      "Content-Type": "application/json",
      Connection: "close",
      ...(init.headers ?? {}),
    },
  });
  const text = await res.text();
  if (!res.ok) throw new Error(`${init.method ?? "GET"} ${path} → ${res.status} ${text.slice(0, 300)}`);
  return text ? JSON.parse(text) : {};
};

const git = (args, opts = {}) =>
  execFileSync("git", args, { cwd: CWD, maxBuffer: 64 * 1024 * 1024, ...opts });

// 远端 sha 是 API 造出来的，本地 git 不认识它，所以不能拿它直接 rev-list。
// 换个锚点：远端那个提交的 tree 一定等于某个本地提交的 tree（内容相同），
// 找到它，从这里往后的提交就是要重放的部分。
const remoteCommit = await api(`/repos/${OWNER}/${REPO}/git/commits/${REMOTE_SHA}`);
const localLog = git(["log", "--format=%H %T", "--all"]).toString().trim().split("\n");

// 正常情况：远端 tip 的 tree 在本地能找到一模一样的提交，那就是锚点。
// 但本地若 rebase/amend 过，远端 tip 是「已不存在于本地历史」的旧树，
// 这时退一步：拿远端 tip 的**父提交**当锚点，把本地那段整体重放一遍并强推覆盖 —— 就是一次安全的改写。
let anchor = null;
let baseRemoteSha = REMOTE_SHA;
let needForce = false;

for (const seed of [remoteCommit, ...(await Promise.all(
  (remoteCommit.parents ?? []).map((p) => api(`/repos/${OWNER}/${REPO}/git/commits/${p.sha}`)),
))]) {
  const hit = localLog.find((line) => line.endsWith(seed.tree.sha));
  if (hit) {
    anchor = hit.split(" ")[0];
    baseRemoteSha = seed.sha;
    needForce = seed.sha !== REMOTE_SHA;
    break;
  }
}

if (!anchor) {
  console.error("远端 tree（连同它的父提交）在本地都找不到——可能本地落后了，先同步。");
  process.exit(1);
}
console.log(
  `锚点：本地 ${anchor.slice(0, 7)} 的 tree 与远端 ${baseRemoteSha.slice(0, 7)} 一致` +
    (needForce ? "（本地改写过历史，将强推覆盖远端 tip）" : ""),
);

const commits = git(["rev-list", "--reverse", `${anchor}..HEAD`]).toString().trim().split("\n").filter(Boolean);
if (commits.length === 0) {
  console.log("没有新提交，远端已是最新。");
  process.exit(0);
}
console.log(`待重放 ${commits.length} 个提交：\n${commits.join("\n")}`);

let parentSha = baseRemoteSha;
for (const sha of commits) {
  const parent = await api(`/repos/${OWNER}/${REPO}/git/commits/${parentSha}`);
  const status = git(["diff-tree", "-r", "--no-commit-id", "--name-status", sha]).toString().trim();
  const tree = [];

  for (const line of status.split("\n").filter(Boolean)) {
    const parts = line.split("\t");
    const kind = parts[0][0];
    if (kind === "D") {
      tree.push({ path: parts[1], mode: "100644", type: "blob", sha: null });
      continue;
    }
    if (kind === "R" || kind === "C") {
      // 重命名拆成「删旧 + 建新」，Git Data API 的 tree 只认最终路径
      tree.push({ path: parts[1], mode: "100644", type: "blob", sha: null });
      parts.splice(0, 2, "M");
    }
    const path = parts[1];
    const mode = git(["ls-tree", sha, "--", path]).toString().split(" ")[0];
    const content = git(["cat-file", "blob", `${sha}:${path}`]);
    const blob = await api(`/repos/${OWNER}/${REPO}/git/blobs`, {
      method: "POST",
      body: JSON.stringify({ content: content.toString("base64"), encoding: "base64" }),
    });
    tree.push({ path, mode, type: "blob", sha: blob.sha });
  }

  const newTree = await api(`/repos/${OWNER}/${REPO}/git/trees`, {
    method: "POST",
    body: JSON.stringify({ base_tree: parent.tree.sha, tree }),
  });

  const message = git(["log", "-1", "--format=%B", sha]).toString().trimEnd();
  const authorName = git(["log", "-1", "--format=%an", sha]).toString().trim();
  const authorEmail = git(["log", "-1", "--format=%ae", sha]).toString().trim();
  const authorDate = git(["log", "-1", "--format=%aI", sha]).toString().trim();

  const commit = await api(`/repos/${OWNER}/${REPO}/git/commits`, {
    method: "POST",
    body: JSON.stringify({
      message,
      tree: newTree.sha,
      parents: [parentSha],
      author: { name: authorName, email: authorEmail, date: authorDate },
      committer: { name: authorName, email: authorEmail, date: authorDate },
    }),
  });
  console.log(`✓ ${sha.slice(0, 7)} → ${commit.sha.slice(0, 7)}  ${message.split("\n")[0]}`);
  parentSha = commit.sha;
}

await api(`/repos/${OWNER}/${REPO}/git/refs/heads/main`, {
  method: "PATCH",
  body: JSON.stringify({ sha: parentSha, force: needForce }),
});
console.log(`\n远端 main 已指向 ${parentSha}`);
process.exitCode = 0;
