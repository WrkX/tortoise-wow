#!/usr/bin/env python3
"""Tests for the Turtle/Vanilla AHBot market-stats builder."""

from __future__ import annotations

import sys
import tempfile
import unittest
from datetime import date
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import build_ahbot_price_stats as builder  # noqa: E402


def write_sql(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8", newline="\n")
    return path


class PercentileTests(unittest.TestCase):
    def test_nearest_rank_uses_observed_values(self) -> None:
        values = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
        self.assertEqual(builder.observed_percentile(values, 10), 10)
        self.assertEqual(builder.observed_percentile(values, 25), 30)
        self.assertEqual(builder.observed_percentile(values, 50), 50)
        self.assertEqual(builder.observed_percentile(values, 75), 80)
        self.assertEqual(builder.observed_percentile(values, 90), 90)

    def test_single_observation_repeats_across_percentiles(self) -> None:
        values = [1234]
        for percentile in (10, 25, 50, 75, 90):
            self.assertEqual(builder.observed_percentile(values, percentile), 1234)

    def test_empty_values_raise(self) -> None:
        with self.assertRaises(ValueError):
            builder.observed_percentile([], 50)


class BuilderEndToEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self._tmpdir.name)

    def tearDown(self) -> None:
        self._tmpdir.cleanup()

    def run_builder(self, *args: str) -> tuple[int, str]:
        output = self.root / "out.sql"
        argv = [str(arg) for arg in args] + ["-o", str(output)]
        code = builder.main(argv)
        text = output.read_text(encoding="utf-8") if output.exists() else ""
        return code, text

    def test_percentiles_and_sample_count(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "\n".join(
                [
                    "-- AHBOT_SNAPSHOT server=nordanaar faction=alliance date=2026-09-01 complete=1 expected_listings=10",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 10);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 20);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 30);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 40);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 50);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 60);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 70);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 80);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 90);",
                    "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 100);",
                ]
            )
            + "\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 0)
        self.assertIn(
            "VALUES (2580, 0, 1, 10, 10, 10, 30, 50, 80, 90, 100)",
            sql,
        )

    def test_multiple_servers_are_aggregated_per_faction(self) -> None:
        nord = write_sql(
            self.root / "nordanaar" / "alliance" / "2026-09-01.sql",
            "-- AHBOT_SNAPSHOT server=nordanaar faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `unit_price`) VALUES (2580, 100);\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `unit_price`) VALUES (2580, 300);\n",
        )
        tel = write_sql(
            self.root / "telabim" / "alliance" / "2026-09-01.sql",
            "-- AHBOT_SNAPSHOT server=telabim faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `unit_price`) VALUES (2580, 500);\n",
        )
        horde = write_sql(
            self.root / "nordanaar" / "horde" / "2026-09-01.sql",
            "-- AHBOT_SNAPSHOT server=nordanaar faction=horde date=2026-09-01\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `unit_price`) VALUES (2580, 9000);\n",
        )
        code, sql = self.run_builder(str(nord), str(tel), str(horde))
        self.assertEqual(code, 0)
        self.assertIn("VALUES (2580, 0, 1, 3, 100, 100, 100, 300, 500, 500, 500)", sql)
        self.assertIn("VALUES (2580, 0, 6, 1, 9000, 9000, 9000, 9000, 9000, 9000, 9000)", sql)
        self.assertIn("nordanaar", sql)
        self.assertIn("telabim", sql)

    def test_missing_days_use_snapshot_count_not_calendar_span(self) -> None:
        day1 = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 10);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 11);\n",
        )
        day2 = write_sql(
            self.root / "nordanaar_alliance_2026-09-02.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-02\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2770, 5);\n",
        )
        day4 = write_sql(
            self.root / "nordanaar_alliance_2026-09-04.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-04\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 12);\n",
        )
        code, sql = self.run_builder(str(day1), str(day2), str(day4))
        self.assertEqual(code, 0)
        # snapshot_count=3 (files), days_seen=2, seen_count=2, listing_count=3
        self.assertIn(
            "INSERT INTO `ahbot_custom_listing_stats` "
            "(`item_id`, `suffix_id`, `auction_house`, `snapshot_count`, `days_seen`, "
            "`seen_count`, `listing_count`) VALUES (2580, 0, 1, 3, 2, 2, 3)",
            sql,
        )
        self.assertNotIn("VALUES (2580, 0, 1, 4,", sql)

    def test_duplicate_rows_count_as_separate_listings(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 0)
        self.assertIn("VALUES (2580, 0, 1, 2, 40, 40, 40, 40, 40, 40, 40)", sql)
        self.assertIn("VALUES (2580, 0, 1, 1, 1, 1, 2)", sql)

    def test_suffix_ids_are_not_merged_with_base_item(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (754, 0, 100);\n"
            "INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (754, 5, 400);\n"
            "INSERT INTO `ahbot_aux_listings` (`item`, `buyout`, `quantity`) VALUES ('754:5', 800, 2);\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 0)
        self.assertIn("VALUES (754, 0, 1, 1, 100, 100, 100, 100, 100, 100, 100)", sql)
        self.assertIn("VALUES (754, 5, 1, 2, 400, 400, 400, 400, 400, 400, 400)", sql)
        self.assertIn("DELETE FROM `ahbot_price` WHERE `item` = '754' AND `auction_house` = '1';", sql)
        self.assertNotIn("WHERE `item` = '754:5'", sql)

    def test_malformed_rows_are_skipped(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01 expected_listings=1\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 25);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (not-a-number, 10);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, -5);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (0, 10);\n"
            "INSERT INTO some_other_table (`item_id`, `price`) VALUES (2580, 99);\n",
        )
        code, sql = self.run_builder(str(snapshot), "--allow-incomplete")
        self.assertEqual(code, 0)
        self.assertIn("VALUES (2580, 0, 1, 1, 25, 25, 25, 25, 25, 25, 25)", sql)
        self.assertNotIn("99", sql.split("ahbot_custom_price_stats", 1)[-1].split("ahbot_price", 1)[0])

    def test_legacy_private_project_insert_lines(self) -> None:
        snapshot = write_sql(
            self.root / "legacy.sql",
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 3999) "
            "ON DUPLICATE KEY UPDATE price = (price + VALUES(price)) / 2;\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2770, 50), (2770, 70);\n",
        )
        code, sql = self.run_builder(
            str(snapshot),
            "--default-faction",
            "alliance",
            "--default-server",
            "nordanaar",
        )
        self.assertEqual(code, 0)
        self.assertIn("VALUES (2580, 0, 1, 1, 3999, 3999, 3999, 3999, 3999, 3999, 3999)", sql)
        self.assertIn("VALUES (2770, 0, 1, 2, 50, 50, 50, 50, 70, 70, 70)", sql)

    def test_deterministic_output(self) -> None:
        first = write_sql(
            self.root / "b_nordanaar_horde_2026-09-02.sql",
            "-- AHBOT_SNAPSHOT faction=horde date=2026-09-02\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2770, 8);\n",
        )
        second = write_sql(
            self.root / "a_nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 4);\n",
        )
        out1 = self.root / "one.sql"
        out2 = self.root / "two.sql"
        self.assertEqual(builder.main([str(first), str(second), "-o", str(out1)]), 0)
        self.assertEqual(builder.main([str(second), str(first), "-o", str(out2)]), 0)
        self.assertEqual(out1.read_bytes(), out2.read_bytes())

    def test_no_truncate_upsert_mode(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n",
        )
        code, sql = self.run_builder(str(snapshot), "--no-truncate")
        self.assertEqual(code, 0)
        self.assertNotIn("TRUNCATE TABLE", sql)
        self.assertIn("ON DUPLICATE KEY UPDATE", sql)
        self.assertIn("`seen_count` = VALUES(`seen_count`)", sql)

    def test_truncate_mode_still_upserts(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 0)
        self.assertIn("TRUNCATE TABLE `ahbot_custom_price_stats`;", sql)
        self.assertIn("TRUNCATE TABLE `ahbot_custom_listing_stats`;", sql)
        self.assertIn("ON DUPLICATE KEY UPDATE", sql)

    def test_incomplete_metadata_fails_without_flag(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01 complete=0\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 1)
        self.assertEqual(sql, "")

    def test_expected_listing_mismatch_fails(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01 expected_listings=5\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n",
        )
        code, _sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 1)

    def test_wotlk_item_ids_are_rejected(self) -> None:
        snapshot = write_sql(
            self.root / "nordanaar_alliance_2026-09-01.sql",
            "-- AHBOT_SNAPSHOT faction=alliance date=2026-09-01\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 40);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (37650, 999);\n"
            "INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (55371, 80);\n",
        )
        code, sql = self.run_builder(str(snapshot))
        self.assertEqual(code, 0)
        self.assertIn("VALUES (2580, 0, 1, 1, 40, 40, 40, 40, 40, 40, 40)", sql)
        self.assertIn("VALUES (55371, 0, 1, 1, 80, 80, 80, 80, 80, 80, 80)", sql)
        self.assertNotIn("37650", sql)

    def test_schema_charset_matches_repo(self) -> None:
        schema = (
            TOOLS_DIR.parents[1]
            / "sql"
            / "characters"
            / "ai_playerbot_ahbot_market_stats.sql"
        )
        text = schema.read_text(encoding="utf-8")
        self.assertIn("DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci", text)
        self.assertIn("`days_seen`", text)
        self.assertIn("`listing_count`", text)
        self.assertNotIn("utf8mb4", text)


class HelperTests(unittest.TestCase):
    def test_item_key_with_suffix(self) -> None:
        self.assertEqual(builder.parse_item_identifier("754:5"), (754, 5))
        self.assertEqual(builder.parse_item_identifier("'754-12'"), (754, 12))
        self.assertEqual(builder.parse_item_identifier("2580"), (2580, 0))

    def test_path_inference(self) -> None:
        meta = builder.infer_meta_from_path(
            Path("snapshots/telabim/horde/20260907.sql"),
            builder.SnapshotMeta(),
        )
        self.assertEqual(meta.server, "telabim")
        self.assertEqual(meta.faction, "horde")
        self.assertEqual(meta.snapshot_date, date(2026, 9, 7))


if __name__ == "__main__":
    unittest.main()
