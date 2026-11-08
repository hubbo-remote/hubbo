// Prints the production bundle's raw and gzipped size: it all lives in ESP32 flash.
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { join, relative } from 'node:path'
import { gzipSync } from 'node:zlib'

const dist = new URL('../dist/', import.meta.url).pathname
const walk = (d: string): string[] => readdirSync(d).flatMap((f) => (statSync(join(d, f)).isDirectory() ? walk(join(d, f)) : [join(d, f)]))
const rows = walk(dist)
  .map((f) => {
    const buf = readFileSync(f)
    return { file: relative(dist, f), raw: buf.length, gz: gzipSync(buf, { level: 9 }).length }
  })
  .sort((a, b) => b.gz - a.gz)
const kb = (n: number) => `${(n / 1024).toFixed(1)} KB`.padStart(9)
console.log('\nfile'.padEnd(46) + 'raw'.padStart(9) + 'gzip'.padStart(9))
for (const r of rows) console.log(r.file.padEnd(45) + kb(r.raw) + kb(r.gz))
const tot = rows.reduce((a, r) => ({ raw: a.raw + r.raw, gz: a.gz + r.gz }), { raw: 0, gz: 0 })
console.log('total'.padEnd(45) + kb(tot.raw) + kb(tot.gz))
