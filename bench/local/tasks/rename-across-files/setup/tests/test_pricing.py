import unittest

from shop import calc_ttl
from shop.cart import Cart
from shop.pricing import TAX_RATE


class PricingTest(unittest.TestCase):
    def test_calc_ttl(self):
        self.assertEqual(calc_ttl([(10.0, 2)]), round(20 * (1 + TAX_RATE), 2))

    def test_discount(self):
        self.assertEqual(calc_ttl([(100.0, 1)], 0.5), 54.0)

    def test_bad_discount(self):
        with self.assertRaises(ValueError):
            calc_ttl([(1.0, 1)], 1.5)

    def test_cart(self):
        cart = Cart()
        cart.add("a", 2.0, 3)
        self.assertEqual(cart.total(), 6.48)


if __name__ == "__main__":
    unittest.main()
