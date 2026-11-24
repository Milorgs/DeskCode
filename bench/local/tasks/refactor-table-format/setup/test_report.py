import unittest

from report import inventory_table, sales_table, staff_table


class ReportTest(unittest.TestCase):
    def test_sales(self):
        expected = (
            "| Region | Units | Revenue |\n"
            "|--------|-------|---------|\n"
            "| North  |    12 |  340.50 |\n"
            "| South  |     3 |   12.00 |"
        )
        self.assertEqual(sales_table([("North", 12, 340.5), ("South", 3, 12)]), expected)

    def test_inventory_sorted_by_sku(self):
        expected = (
            "| SKU  | Name        | Qty |\n"
            "|------|-------------|-----|\n"
            "| A-01 | Widget      |   7 |\n"
            "| B-22 | Large gizmo | 120 |"
        )
        items = {"B-22": ("Large gizmo", 120), "A-01": ("Widget", 7)}
        self.assertEqual(inventory_table(items), expected)

    def test_staff(self):
        expected = (
            "| Name | Role     | Hours |\n"
            "|------|----------|-------|\n"
            "| Ana  | Engineer |  37.5 |\n"
            "| Bo   | Designer |    40 |"
        )
        people = [{"name": "Ana", "role": "Engineer", "hours": 37.5},
                  {"name": "Bo", "role": "Designer", "hours": 40}]
        self.assertEqual(staff_table(people), expected)


if __name__ == "__main__":
    unittest.main()
