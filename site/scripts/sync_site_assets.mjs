import { cp, mkdir, readdir, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';

const siteRoot = resolve(new URL('..', import.meta.url).pathname);
const repoRoot = resolve(siteRoot, '..');

async function copyIfPresent(from, to) {
  try {
    await stat(from);
  } catch {
    return;
  }
  await mkdir(dirname(to), { recursive: true });
  await cp(from, to, { recursive: true, force: true });
}

async function copyDirectoryWithoutMacMetadata(from, to) {
  try {
    const sourceStat = await stat(from);
    if (!sourceStat.isDirectory()) {
      return;
    }
  } catch {
    return;
  }

  await mkdir(to, { recursive: true });
  const entries = await readdir(from, { withFileTypes: true });
  for (const entry of entries) {
    if (entry.name === '.DS_Store') {
      continue;
    }
    const source = join(from, entry.name);
    const target = join(to, entry.name);
    if (entry.isDirectory()) {
      await copyDirectoryWithoutMacMetadata(source, target);
    } else if (entry.isFile()) {
      await mkdir(dirname(target), { recursive: true });
      await cp(source, target, { force: true });
    }
  }
}

await copyDirectoryWithoutMacMetadata(resolve(repoRoot, 'docs/images'), resolve(siteRoot, 'public/images'));
await copyIfPresent(resolve(repoRoot, 'web/assets/logo.png'), resolve(siteRoot, 'public/logo.png'));
await copyIfPresent(resolve(repoRoot, 'docs/catalog'), resolve(siteRoot, 'public/catalog'));
await copyDirectoryWithoutMacMetadata(resolve(repoRoot, 'docs/languages/template'), resolve(siteRoot, 'public/languages/template'));
await copyIfPresent(resolve(repoRoot, 'docs/CNAME'), resolve(siteRoot, 'public/CNAME'));
