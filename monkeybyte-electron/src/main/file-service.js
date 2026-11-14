import { randomUUID } from 'node:crypto';
import { promises as fs } from 'node:fs';
import path from 'node:path';

export const MAX_READ_BYTES = 5 * 1024 * 1024;
export const MAX_WRITE_BYTES = 25 * 1024 * 1024;

function normalizeRelative(relativePath) {
  if (typeof relativePath !== 'string' || relativePath.includes('\0')) {
    throw new Error('Invalid workspace-relative path.');
  }
  if (path.isAbsolute(relativePath)) {
    throw new Error('Absolute paths are not accepted here.');
  }
  return relativePath;
}

function assertContained(root, candidate) {
  const relative = path.relative(root, candidate);
  if (relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error('The requested path escapes the workspace.');
  }
}

function unresolvedPath(root, relativePath) {
  if (typeof root !== 'string' || !path.isAbsolute(root)) {
    throw new Error('No valid workspace is selected.');
  }
  const relative = normalizeRelative(relativePath);
  const absolute = path.resolve(root, relative);
  assertContained(path.resolve(root), absolute);
  return absolute;
}

async function realDirectory(directory) {
  const resolved = await fs.realpath(directory);
  const stat = await fs.stat(resolved);
  if (!stat.isDirectory()) {
    throw new Error('The workspace is not a directory.');
  }
  return resolved;
}

async function existingPath(root, relativePath) {
  const absolute = unresolvedPath(root, relativePath);
  const [canonicalRoot, canonicalTarget] = await Promise.all([
    realDirectory(root),
    fs.realpath(absolute),
  ]);
  assertContained(canonicalRoot, canonicalTarget);
  return canonicalTarget;
}

export async function validateWorkspace(root) {
  return realDirectory(path.resolve(root));
}

export async function listDirectory(root, relativePath = '', showHidden = false) {
  const directory = await existingPath(root, relativePath);
  const entries = await fs.readdir(directory, { withFileTypes: true });
  const visible = entries.filter((entry) => showHidden || !entry.name.startsWith('.'));
  const mapped = visible.map((entry) => ({
    name: entry.name,
    path: path.join(relativePath, entry.name).split(path.sep).join('/'),
    isDirectory: entry.isDirectory(),
    isSymbolicLink: entry.isSymbolicLink(),
  }));
  return mapped.sort((left, right) => {
    if (left.isDirectory !== right.isDirectory) {
      return left.isDirectory ? -1 : 1;
    }
    return left.name.localeCompare(right.name, undefined, { sensitivity: 'base' });
  });
}

export async function readTextFile(root, relativePath) {
  const filename = await existingPath(root, relativePath);
  const stat = await fs.stat(filename);
  if (!stat.isFile()) {
    throw new Error('The selected path is not a file.');
  }
  if (stat.size > MAX_READ_BYTES) {
    throw new Error('Files larger than 5 MB cannot be opened in the editor.');
  }
  const bytes = await fs.readFile(filename);
  if (bytes.subarray(0, 8192).includes(0)) {
    throw new Error('Binary files must be opened with their system application.');
  }
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    throw new Error('The file is not valid UTF-8 text.');
  }
}

export async function writeTextFile(root, relativePath, content) {
  if (!relativePath) {
    throw new Error('A file path is required.');
  }
  if (typeof content !== 'string' || Buffer.byteLength(content, 'utf8') > MAX_WRITE_BYTES) {
    throw new Error('Text files larger than 25 MB cannot be saved.');
  }

  const requested = unresolvedPath(root, relativePath);
  const canonicalRoot = await realDirectory(root);
  const canonicalParent = await realDirectory(path.dirname(requested));
  assertContained(canonicalRoot, canonicalParent);

  let mode = 0o666;
  try {
    const stat = await fs.lstat(requested);
    if (stat.isSymbolicLink() || !stat.isFile()) {
      throw new Error('Only regular, non-symlink files can be saved.');
    }
    const canonicalTarget = await fs.realpath(requested);
    assertContained(canonicalRoot, canonicalTarget);
    mode = stat.mode;
  } catch (error) {
    if (error?.code !== 'ENOENT') {
      throw error;
    }
  }

  const temporary = path.join(
    canonicalParent,
    `.monkeybyte-${process.pid}-${randomUUID()}.tmp`,
  );
  try {
    await fs.writeFile(temporary, content, { encoding: 'utf8', mode, flag: 'wx' });
    await fs.rename(temporary, requested);
  } catch (error) {
    await fs.rm(temporary, { force: true }).catch(() => {});
    throw error;
  }
}

export async function resolveWorkspacePath(root, relativePath = '') {
  return existingPath(root, relativePath);
}

export async function createFolder(root, relativePath) {
  if (!relativePath) throw new Error('A folder path is required.');
  const requested = unresolvedPath(root, relativePath);
  const canonicalRoot = await realDirectory(root);
  const canonicalParent = await realDirectory(path.dirname(requested));
  assertContained(canonicalRoot, canonicalParent);
  await fs.mkdir(requested, { recursive: false });
}

export async function createTextFile(root, relativePath, content = '') {
  if (!relativePath) throw new Error('A file path is required.');
  const requested = unresolvedPath(root, relativePath);
  const canonicalRoot = await realDirectory(root);
  const canonicalParent = await realDirectory(path.dirname(requested));
  assertContained(canonicalRoot, canonicalParent);
  if (typeof content !== 'string' || Buffer.byteLength(content, 'utf8') > MAX_WRITE_BYTES) {
    throw new Error('Text files larger than 25 MB cannot be created.');
  }
  await fs.writeFile(requested, content, { encoding: 'utf8', flag: 'wx', mode: 0o666 });
}

export async function renameWorkspacePath(root, oldPath, newPath) {
  if (!oldPath || !newPath) throw new Error('Both source and destination are required.');
  const source = unresolvedPath(root, oldPath);
  const destination = unresolvedPath(root, newPath);
  const canonicalRoot = await realDirectory(root);
  const [canonicalSourceParent, canonicalDestinationParent] = await Promise.all([
    realDirectory(path.dirname(source)),
    realDirectory(path.dirname(destination)),
  ]);
  assertContained(canonicalRoot, canonicalSourceParent);
  assertContained(canonicalRoot, canonicalDestinationParent);
  const sourceStat = await fs.lstat(source);
  if (sourceStat.isSymbolicLink()) throw new Error('Symbolic links cannot be renamed here.');
  const canonicalSource = await fs.realpath(source);
  assertContained(canonicalRoot, canonicalSource);
  try {
    await fs.lstat(destination);
    throw new Error('The destination already exists.');
  } catch (error) {
    if (error?.code !== 'ENOENT') throw error;
  }
  await fs.rename(source, destination);
}

export async function deleteWorkspacePath(root, relativePath) {
  if (!relativePath) throw new Error('Refusing to delete the workspace root.');
  const requested = unresolvedPath(root, relativePath);
  const canonicalRoot = await realDirectory(root);
  const canonicalParent = await realDirectory(path.dirname(requested));
  assertContained(canonicalRoot, canonicalParent);
  const stat = await fs.lstat(requested);
  if (stat.isSymbolicLink()) {
    await fs.unlink(requested);
    return;
  }
  const canonicalTarget = await fs.realpath(requested);
  assertContained(canonicalRoot, canonicalTarget);
  if (canonicalTarget === canonicalRoot) throw new Error('Refusing to delete the workspace root.');
  await fs.rm(requested, { recursive: true, force: false });
}

export async function importWorkspacePaths(root, destinationDirectory, sources) {
  const destination = await existingPath(root, destinationDirectory);
  const stat = await fs.stat(destination);
  if (!stat.isDirectory()) throw new Error('The import destination is not a folder.');
  if (!Array.isArray(sources) || sources.length === 0 || sources.length > 100) {
    throw new Error('Select between 1 and 100 files to import.');
  }
  for (const source of sources) {
    if (typeof source !== 'string' || !path.isAbsolute(source)) {
      throw new Error('Import sources must be absolute paths.');
    }
    const sourceStat = await fs.stat(source);
    if (!sourceStat.isFile()) throw new Error('Only regular files can be imported.');
    const target = path.join(destination, path.basename(source));
    assertContained(destination, target);
    await fs.copyFile(source, target, fs.constants.COPYFILE_EXCL);
  }
}
