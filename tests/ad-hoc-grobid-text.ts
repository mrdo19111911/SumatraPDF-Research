// GROBID text helpers in src/GrobidText.cpp (grobid-text action). Expected values follow the
// Java code: FeatureFactory, TextUtilities, UnicodeUtil.normaliseText, Lexicon,
// GrobidDefaultAnalyzer.tokenize.
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

const T = "true",
  F = "false";

const kCases: [fn: string, arg: string, want: string][] = [
  // StringTokenizer(normaliseText(s), delimiters, true)
  ["Tokenize", "Smith, J. (2019). Title-word", "Smith|,| |J|.| |(|2019|)|.| |Title|-|word"],
  ["Tokenize", "a\u2013b", "a|-|b"],
  ["Tokenize", "x\u00A0y", "x| |y"],
  ["Tokenize", "x\ty", "x| |y"],
  ["Tokenize", "intel\u00B7lig\u00E8ncia", "intel\u00B7lig\u00E8ncia"],
  ["Tokenize", "a \u00B7b", "a| |\u2022|b"],
  ["Tokenize", "\u5A01\u00B7\u5EC9", "\u5A01|\u2022|\u5EC9"],
  ["Tokenize", "x\r\ny", "x|\n|y"],
  ["Tokenize", "x\ry", "x|\n|y"],
  ["Tokenize", "e-mail@x.org", "e|-|mail|@|x|.|org"],
  ["Tokenize", "50% & \u03B1", "50%| |&| |\u03B1"],
  ["Tokenize", "\uFF08x\uFF09", "(|x|)"],
  ["Tokenize", "a\u200Cb", "a|\u200C|b"],
  ["Tokenize", "a\u2028b", "a| |b"],
  ["Tokenize", "a\u0085b", "a|\n|b"],
  ["Tokenize", "a\u2043b", "a|\u2022|b"],
  ["Tokenize", "\u201CQuoted\u201D \u2018x\u2019", "\u201C|Quoted|\u201D| |\u2018|x|\u2019"],
  ["Tokenize", "p.\u00A012\u201315", "p|.| |12|-|15"],
  ["Tokenize", "word", "word"],
  ["Tokenize", "", ""],
  ["Normalise", "a\u2010b\u2011c\u2212d", "a-b-c-d"],
  ["Normalise", "\u27EEx\u27EF \u2985y\u2986", "(x) (y)"],
  ["Normalise", "a\r\rb", "a\n\nb"],
  ["Normalise", "a\u2223b\u203Fc", "a|b_c"],
  ["Normalise", "CuSO4\u00B75H2O", "CuSO4\u00B75H2O"],
  ["Normalise", "\u00B7end", "\u2022end"],
  ["PunctProfile", "Smith, J. (2019). Title: \u00ABx\u00BB", ",.().:\u00AB\u00BB"],
  ["PunctProfile", "abc", ""],
  ["PunctProfile", "a & b % c", ""],
  ["Prefix", "2,hello", "he"],
  ["Prefix", "4,ab", "ab"],
  ["Prefix", "1,\u00E4ngel", "\u00E4"],
  ["Prefix", "0,abc", ""],
  ["Suffix", "2,hello", "lo"],
  ["Suffix", "3,na\u00EFve", "\u00EFve"],
  ["Suffix", "4,ab", "ab"],
  ["IsYear", "2019", T],
  ["IsYear", ",999", T],
  ["IsYear", "1899a", T],
  ["IsYear", "x1,234", T],
  ["IsYear", "3019", F],
  ["IsYear", "201", F],
  ["AllDigits", "123", T],
  ["AllDigits", "12a", F],
  ["AllDigits", "\u0661\u0662", F],
  ["AllDigits", "123\n", T],
  ["HasDigit", "ab3", T],
  ["HasDigit", "abc", F],
  ["HasDigit", "x\u0663", T],
  ["HasDigit", "x\u00B2", F],
  ["AllCapital", "ABC", T],
  ["AllCapital", "AbC", F],
  ["AllCapital", "2019", T],
  ["AllCapital", "\u00C9COLE", T],
  ["AllCapital", "\u00C9cole", F],
  ["IsUpperFirst", "Hello", T],
  ["IsUpperFirst", "hello", F],
  ["IsUpperFirst", "\u00C9lan", T],
  ["IsUpperFirst", "1abc", F],
  ["IsPunct", ",.;", T],
  ["IsPunct", "?", T],
  ["IsPunct", ",a", F],
  ["IsPunct", "!", F],
  ["IsHttp", "https", T],
  ["IsHttp", "xhttpy", T],
  ["IsHttp", "htt", F],
  ["IsEmail", "john.doe@example.com", T],
  ["IsEmail", "a@b.c", F],
  ["IsEmail", "x@[1.2.3.4]", T],
  ["IsEmail", "x@1.2.3.4", T],
  ["IsEmail", "x@1.2.3.256", F],
  ["IsEmail", "a;b@x.org", T],
  ["IsEmail", ".a@x.org", F],
  ["IsEmail", "a@x.org.", T],
  ["IsEmail", "@x.org", F],
  ["IsEmail", "a@x..org", F],
  ["IsMonth", "January", T],
  ["IsMonth", "MAY", T],
  ["IsMonth", "dec", T],
  ["IsMonth", "Sept", F],
  ["FilterLine", "", T],
  ["FilterLine", "foo @IMAGE bar", T],
  ["FilterLine", "@PAGE", T],
  ["FilterLine", "text", F],
  ["Pattern", "Hello, World 42!", "helloworld"],
  ["Pattern", "\u00DCn\u00EFcode", "ncode"],
  ["Lower", "\u00C0BC", "\u00E0bc"],
  ["Lower", "MiXeD", "mixed"],
  ["InDictionary", "The", T],
  ["InDictionary", "the.", T],
  ["InDictionary", " the ", T],
  ["InDictionary", "well-known", T],
  ["InDictionary", "well-zzzqx", F],
  ["InDictionary", "zzzqx", F],
  ["InDictionary", "-", F],
  ["InDictionary", "", F],
  ["IsFirstName", "John", T],
  ["IsFirstName", "Zzyzxq", F],
  ["IsLastName", "Smith", T],
  ["IsLastName", "Zzyzxq", F],
  // FeatureFactory.linearScaling: the int overload computes in float
  ["LinearScaling", "5,10,12", "6"],
  ["LinearScaling", "10,10,12", "12"],
  ["LinearScaling", "-1,10,12", "0"],
  ["LinearScaling", "3.3,10,10", "3"],
  ["LinearScaling", "29,100,100", "28"],
  ["LinearScalingInt", "29,100,100", "29"],
  ["LinearScalingInt", "7,10,10", "7"],
  ["LinearScalingInt", "11,10,10", "10"],
];

export async function testit(): Promise<void> {
  const failures: string[] = [];
  await withControlledSumatra(EXE, async (client) => {
    for (const [fn, arg, want] of kCases) {
      const got = String((await client.request(ControlCommand.TestResearchTools, ["grobid-text", fn, arg]))[1]);
      if (got !== want)
        failures.push(`${fn}(${JSON.stringify(arg)}) = ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
    }
    for (const file of ["words-en.txt", "names-first.txt", "names-last.txt", "citation.wapiti"]) {
      const size = String((await client.request(ControlCommand.TestResearchTools, ["grobid-text", "Data", file]))[1]);
      if (!(Number(size) > 1000)) failures.push(`GrobidDataTemp(${file}) size ${size}`);
    }
  });
  console.log(`${kCases.length - failures.length}/${kCases.length} cases pass`);
  if (failures.length) throw new Error(failures.join("\n"));
}

if (import.meta.main) {
  await runStandalone(testit);
}
