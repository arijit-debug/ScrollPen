// Writes fixtures/sample.pdf: three letter-size pages, each with a big heading and a red
// ruler every 100 pt, so tests and people can see where ink sits. Run once:
//   node support/make-pdf.mjs
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const pages = 3;
const objects = [];
const add = (body) => objects.push(body) && objects.length;

const catalog = add(''); // filled below
const pagesObj = add('');
const font = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
const kids = [];
for (let i = 1; i <= pages; i++) {
  let ops = `BT /F1 36 Tf 72 700 Td (ScrollPen sample, page ${i}) Tj ET\n1 0 0 RG 2 w\n`;
  for (let y = 100; y < 792; y += 100) ops += `0 ${y} m 612 ${y} l S\n`;
  ops += `BT /F1 14 Tf 72 640 Td (Draw on this page, then scroll: the ink stays with the text.) Tj ET\n`;
  const content = add(`<< /Length ${Buffer.byteLength(ops)} >>\nstream\n${ops}endstream`);
  kids.push(
    add(`<< /Type /Page /Parent ${pagesObj} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 ${font} 0 R >> >> /Contents ${content} 0 R >>`),
  );
}
objects[catalog - 1] = `<< /Type /Catalog /Pages ${pagesObj} 0 R >>`;
objects[pagesObj - 1] = `<< /Type /Pages /Kids [${kids.map((k) => `${k} 0 R`).join(' ')}] /Count ${pages} >>`;

let pdf = '%PDF-1.4\n';
const offsets = [];
objects.forEach((body, i) => {
  offsets.push(Buffer.byteLength(pdf));
  pdf += `${i + 1} 0 obj\n${body}\nendobj\n`;
});
const xref = Buffer.byteLength(pdf);
pdf += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
for (const o of offsets) pdf += `${String(o).padStart(10, '0')} 00000 n \n`;
pdf += `trailer\n<< /Size ${objects.length + 1} /Root ${catalog} 0 R >>\nstartxref\n${xref}\n%%EOF\n`;

// sample.pdfdata is the same file under a name download managers don't grab (Internet
// Download Manager intercepts .pdf even from 127.0.0.1 and answers the page with nothing).
const fixtures = join(dirname(fileURLToPath(import.meta.url)), '..', 'fixtures');
for (const name of ['sample.pdf', 'sample.pdfdata']) {
  writeFileSync(join(fixtures, name), pdf, 'latin1');
  console.log(join(fixtures, name));
}
