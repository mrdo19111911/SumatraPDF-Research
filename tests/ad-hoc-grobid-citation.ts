// GROBID citation port (src/GrobidCitation.cpp): feature rows and fields of one raw reference.
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

// FeaturesVectorCitation.printVector(): 29 features + label "0"
const nColumns = 30;
const col = {
  line: 10,
  year: 19,
  location: 18,
  journal: 23,
  conference: 24,
  publisher: 25,
  identifier: 26,
  relpos: 28,
};

type Ref = { title: string; authors: string; year: string; venue: string; doi: string };

type Case = {
  raw: string;
  title?: string;
  authors?: string;
  authorsPrefix?: string;
  year?: string;
  venue?: RegExp;
  doi?: string;
};

const betancourt =
  "Betancourt, M. The fundamental incompatibility of scalable Hamiltonian Monte Carlo and naive data subsampling. In Proceedings of the 32nd International Conference on Machine Learning, pp. 533-540, 2015.";

const cases: Case[] = [
  {
    raw: betancourt,
    title: "The fundamental incompatibility of scalable Hamiltonian Monte Carlo and naive data subsampling",
    authors: "Betancourt, M.",
    year: "2015",
    venue: /International Conference on Machine Learning/,
  },
  {
    raw: "[12] W. Su, S. Boyd, and E. J. Candes. A differential equation for modeling Nesterov's accelerated gradient method: theory and insights. Journal of Machine Learning Research, 17(153):1-43, 2016.",
    title: "A differential equation for modeling Nesterov's accelerated gradient method: theory and insights",
    authors: "Su, W.; Boyd, S.; Candes, E. J.",
    year: "2016",
    venue: /^Journal of Machine Learning Research$/,
  },
  {
    raw: "Smith, J. A., & Doe, R. (2013a). Title here. Journal Name, 4(2), 1-10. https://doi.org/10.1000/xyz123",
    authorsPrefix: "Smith, J. A.; Doe, R.",
    year: "2013",
    doi: "10.1000/xyz123",
  },
  {
    raw: "Jacob Devlin, Ming-Wei Chang, Kenton Lee, and Kristina Toutanova. 2019. BERT: Pre-training of Deep Bidirectional Transformers for Language Understanding. In Proceedings of the 2019 Conference of the North American Chapter of the Association for Computational Linguistics: Human Language Technologies, Volume 1 (Long and Short Papers), pages 4171-4186, Minneapolis, Minnesota. Association for Computational Linguistics.",
    title: "BERT: Pre-training of Deep Bidirectional Transformers for Language Understanding",
    authorsPrefix: "Devlin, J.; Chang, M.",
    year: "2019",
    venue: /North American Chapter of the Association for Computational Linguistics/,
  },
  {
    raw: "Nocedal, J. and Wright, S. J. Numerical Optimization. Springer, New York, 2nd edition, 2006.",
    title: "Numerical Optimization",
    authors: "Nocedal, J.; Wright, S. J.",
    year: "2006",
  },
  {
    // labeled <booktitle> only: the title of a book, as in GROBID's TEI <monogr>
    raw: ". Arnold, V.I. Mathematical Methods of Classical Mechanics ; Springer: New York, NY, USA, 1989; Volume 60.",
    title: "Mathematical Methods of Classical Mechanics",
    authors: "Arnold, V. I.",
    year: "1989",
    venue: /^$/,
  },
  {
    raw: "Mrugala, R.; Nulton, J.D. Statistical approach to the geometric structure of thermodynamics. Phys. Rev. A 1990, 41, 3156-3160. doi:10.1103/PhysRevA.41.3156",
    title: "Statistical approach to the geometric structure of thermodynamics",
    authors: "Mrugala, R.; Nulton, J. D.",
    year: "1990",
    venue: /^Phys\. Rev\. A$/,
    doi: "10.1103/PhysRevA.41.3156",
  },
  {
    raw: "Y. Nesterov. A method for solving the convex programming problem with convergence rate O(1/k^2). Dokl. Akad. Nauk SSSR, 269:543-547, 1983.",
    authors: "Nesterov, Y.",
    year: "1983",
  },
];

export async function testit(): Promise<void> {
  const failures: string[] = [];
  await withControlledSumatra(EXE, async (client) => {
    async function probe(action: string, a = "", b = "", c = "") {
      const result = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
      if (result[0] !== 0) throw new Error(`${action} ${a}: ${String(result[1]).slice(0, 300)}`);
      return String(result[1]);
    }

    // one row per non-space token, 30 columns, label "0"
    const rows = (await probe("grobid-citfeatures", betancourt)).split("\n").filter((r) => r.length > 0);
    console.log(`citfeatures: ${rows.length} rows`);
    if (rows.length !== 37) failures.push(`citfeatures: ${rows.length} rows, want 37`);
    const byToken = new Map<string, string[]>();
    for (const row of rows) {
      const cols = row.split(" ");
      if (cols.length !== nColumns || cols[nColumns - 1] !== "0") {
        failures.push(`citfeatures: bad row '${row}'`);
        break;
      }
      if (!byToken.has(cols[0]!)) byToken.set(cols[0]!, cols);
    }
    const first = rows[0] ?? "";
    const wantFirst = "Betancourt betancourt B Be Bet Beta t rt urt ourt LINESTART INITCAP NODIGIT 0";
    if (!first.startsWith(wantFirst)) failures.push(`citfeatures: first row '${first}'`);
    const expectFlag = (tok: string, c: number, v: string) => {
      const cols = byToken.get(tok);
      if (cols?.[c] !== v) failures.push(`citfeatures: '${tok}' column ${c} = ${cols?.[c]}, want ${v}`);
    };
    expectFlag("2015", col.year, "1");
    expectFlag("2015", col.line, "LINEIN");
    expectFlag("Conference", col.conference, "1");
    expectFlag("Machine", col.conference, "1");
    expectFlag("Hamiltonian", col.conference, "0");
    expectFlag(".", col.line, "LINEIN");
    const last = rows[rows.length - 1]?.split(" ") ?? [];
    if (last[0] !== "." || last[col.line] !== "LINEEND" || last[col.relpos] !== "11")
      failures.push(`citfeatures: last row '${rows[rows.length - 1]}'`);

    const doiRows = (await probe("grobid-citfeatures", cases[2]!.raw)).split("\n");
    const xyz = doiRows.find((r) => r.startsWith("xyz "))?.split(" ");
    if (xyz?.[col.identifier] !== "1") failures.push(`citfeatures: doi token not flagged as identifier`);
    const journalRows = (await probe("grobid-citfeatures", cases[1]!.raw)).split("\n");
    const research = journalRows.find((r) => r.startsWith("Research "))?.split(" ");
    if (research?.[col.journal] !== "1") failures.push(`citfeatures: 'Research' not flagged as journal`);
    const aclRows = (await probe("grobid-citfeatures", cases[3]!.raw)).split("\n");
    const minneapolis = aclRows.find((r) => r.startsWith("Minneapolis "))?.split(" ");
    if (minneapolis?.[col.location] !== "1") failures.push(`citfeatures: 'Minneapolis' not flagged as location`);

    for (const c of cases) {
      const refs: Ref[] = JSON.parse(await probe("grobid-citation", c.raw));
      const r = refs[0]!;
      console.log(JSON.stringify(r));
      const tag = c.raw.slice(0, 30);
      if (c.title !== undefined && r.title !== c.title) failures.push(`${tag}: title '${r.title}'`);
      if (c.authors !== undefined && r.authors !== c.authors) failures.push(`${tag}: authors '${r.authors}'`);
      if (c.authorsPrefix !== undefined && !r.authors.startsWith(c.authorsPrefix))
        failures.push(`${tag}: authors '${r.authors}'`);
      if (c.year !== undefined && r.year !== c.year) failures.push(`${tag}: year '${r.year}'`);
      if (c.venue !== undefined && !c.venue.test(r.venue)) failures.push(`${tag}: venue '${r.venue}'`);
      if (c.doi !== undefined && r.doi !== c.doi) failures.push(`${tag}: doi '${r.doi}'`);
    }
  });
  if (failures.length > 0) throw new Error(failures.join("\n"));
  console.log("PASS");
}

if (import.meta.main) {
  await runStandalone(testit);
}
