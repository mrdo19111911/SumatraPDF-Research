import unittest

from title_detector import title_candidates as detect_candidates, title_parts as detect_parts


def title_candidates(*args, **kwargs):
    return detect_candidates(*args, **kwargs, mode="and")


def title_parts(*args, **kwargs):
    return detect_parts(*args, **kwargs, mode="and")


class TitleDetectorTests(unittest.TestCase):
    def test_structure_formats(self):
        from title_detector import structured_title

        title = "A theorem on compact spaces"
        for raw in (
            f"A. Author, {title}, Math. Journal 8 (1999), 20-30.",
            f"Author, A.: {title}. Math. Journal 8, 20-30 (1999).",
            f"Alice Author and Bob Writer. {title}. Journal of Geometry, 1999.",
            f"A. Author (1999) {title}. Journal of Geometry 8, 20-30.",
            f"K.-H. Author. {title}. Journal of Geometry, 1999.",
        ):
            with self.subTest(raw=raw):
                self.assertEqual(structured_title(raw), title)

    def test_structure_preserves_title(self):
        from title_detector import structured_title

        for title in (
            "Convex analysis",
            "Density: an almost every result",
            "Representations of GL(n,F), where F is a local field",
            "Estimates in small intervals. III",
            "Journal rankings and conference selection",
        ):
            with self.subTest(title=title):
                raw = f"A. Author (1999) {title}. Journal of Geometry 8, 20-30."
                self.assertEqual(structured_title(raw), title)

    def test_structure_unknown(self):
        from title_detector import structured_title

        for raw in ("", "1999, 20-30", "Unstructured citation without delimiters"):
            self.assertEqual(structured_title(raw), "")

    def test_structure_author_boundaries(self):
        from title_detector import structured_title

        for raw, expected in (
            ("Baker, D., Writer, S.: A Course in Geometry. Press, 2001.", "A Course in Geometry"),
            ("Alice Baker, Bob Writer, and Carol Scholar. Compact metric spaces. Journal, 2001.", "Compact metric spaces"),
            ("J. S. Baker. Algebraic Groups: A Theory of Schemes. Press, 2001.", "Algebraic Groups: A Theory of Schemes"),
            ("A. Baker, Compact metric spaces, Regional J. Math. 9 (2001), 1-10.", "Compact metric spaces"),
        ):
            with self.subTest(raw=raw):
                self.assertEqual(structured_title(raw), expected)

    def test_structure_colon_in_title(self):
        from title_detector import structured_title

        raw = "A. Baker, B. Writer, Prevalence: an almost every theorem, Bull. Math. 7 (1992), 1-10."
        self.assertEqual(structured_title(raw), "Prevalence: an almost every theorem")

    def test_venue_markers(self):
        for marker in ("Jour.", "Jour", "Journal", "journals", "J.", "Proceeding", "Proceedings", "Conference", "conferences", "Conf.", "CONF"):
            with self.subTest(marker=marker):
                venue = f"{marker} of exceptionally long publication information"
                raw = f"A. Author, Real paper title on spaces, {venue}, 2019."
                self.assertNotIn(venue, title_parts(raw, ","))
                self.assertEqual(title_candidates(raw)[0], "Real paper title on spaces")

    def test_word_boundaries(self):
        for title in ("Journey through metric spaces", "Conformal structures on manifolds", "Adjoint operators on Hilbert spaces"):
            with self.subTest(title=title):
                self.assertEqual(title_candidates(f"Author, {title}, 2019.")[0], title)

    def test_case_and_punctuation(self):
        for venue in ("International JOURNAL of Mathematics", "(Conf.) Proceedings of Mathematics", "SIAM J. Optimization"):
            with self.subTest(venue=venue):
                self.assertNotIn(venue, title_parts(f"Author, Real paper title, {venue}, 2020.", ","))

    def test_author_initials(self):
        self.assertNotIn("J. Smith and A. Author", title_parts("J. Smith and A. Author, Real paper title, 2020.", ","))

    def test_baseline_switch(self):
        venue = "Journal of exceptionally long publication information"
        raw = f"Author, Real paper title, {venue}, 2020."
        self.assertEqual(title_candidates(raw, filter_venues=False)[0], venue)

    def test_numeric_and_doi_exclusion(self):
        self.assertEqual(title_candidates("Author, 123456789, doi:10.1234/longidentifier."), [])

    def test_empty_reference(self):
        self.assertEqual(title_candidates(""), [])

    def test_strict_length_boundary(self):
        for separator in (",", "."):
            for size in (19, 20, 21):
                with self.subTest(separator=separator, size=size):
                    title = "T" * size
                    raw = f"Author{separator} {title}{separator} 2020."
                    self.assertEqual(title in title_parts(raw, separator), size > 20)

    def test_length_after_trimming(self):
        self.assertNotIn("T" * 20, title_parts("Author,    " + "T" * 20 + "   ., 2020.", ","))
        self.assertIn("T" * 21, title_parts("Author,    " + "T" * 21 + "   ., 2020.", ","))

    def test_unicode_character_count(self):
        self.assertNotIn("ắ" * 20, title_parts("Author, " + "ắ" * 20 + ", 2020.", ","))
        self.assertIn("ắ" * 21, title_parts("Author, " + "ắ" * 21 + ", 2020.", ","))

    def test_or_rules(self):
        for title, expected in (
            ("Convex analysis", True),
            ("A theorem on compact spaces", True),
            ("Journal of geometry for researchers", True),
            ("Conf geometry", False),
        ):
            with self.subTest(title=title):
                self.assertEqual(title in detect_parts(f"Author, {title}, 2020.", ",", mode="or"), expected)

    def test_or_separators(self):
        title = "A theorem on compact spaces"
        for separator in (",", "."):
            with self.subTest(separator=separator):
                self.assertIn(title, detect_candidates(f"Author{separator} {title}{separator} 2020"))

    def test_long_venue_veto(self):
        for marker in ("Jour", "Journal", "Proce", "Proc", "Proceed", "Proceeding", "Proceedings", "Conference", "Conf"):
            for separator in (",", "."):
                venue = f"{marker} of exceptionally long geometry publications"
                with self.subTest(marker=marker, separator=separator):
                    self.assertNotIn(venue, detect_parts(f"Author{separator} {venue}{separator} 2020", separator))

    def test_process_is_not_proceedings(self):
        for title in ("Processes on compact metric spaces", "Procedures for computing curvature"):
            self.assertIn(title, detect_parts(f"Author, {title}, 2020", ","))

    def test_more_venue_markers(self):
        from title_detector import VENUE, structured_title

        title = "A theorem on compact metric spaces"
        for marker in ("Ann.", "ANN", "Annals", "IEEE", "ieee", "trans", "Trans.", "Transactions"):
            venue = f"{marker} of exceptionally long geometry publications"
            raw = f"A. Author, {title}, {venue}, 2020."
            with self.subTest(marker=marker):
                self.assertIsNotNone(VENUE.search(venue))
                self.assertNotIn(venue, detect_parts(raw, ","))
                self.assertEqual(detect_candidates(raw)[0], title)
                self.assertEqual(structured_title(raw), title)

    def test_more_venue_boundaries(self):
        for title in ("Annular structures on manifolds", "Transport on compact metric spaces", "Transversality in geometric analysis"):
            self.assertIn(title, detect_parts(f"Author, {title}, 2020", ","))

    def test_volume_marker(self):
        from title_detector import VENUE

        title = "A theorem on compact metric spaces"
        for marker in ("Vol", "Vol.", "VOL", "Volume", "volume"):
            venue = f"{marker} 37 of exceptionally long publication information"
            with self.subTest(marker=marker):
                self.assertIsNotNone(VENUE.search(venue))
                self.assertNotIn(venue, detect_parts(f"Author, {title}, {venue}, 2020.", ","))
        for word in ("Volterra", "Volumetric", "Volatility"):
            self.assertIsNone(VENUE.search(f"{word} methods on compact spaces"))


if __name__ == "__main__":
    unittest.main()
