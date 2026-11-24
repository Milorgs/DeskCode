"""Shopping cart."""
from shop import pricing


class Cart:
    def __init__(self):
        self.lines = []

    def add(self, name, unit_price, quantity=1):
        self.lines.append((name, unit_price, quantity))

    def total(self, discount=0.0):
        # calc_ttl expects (unit_price, quantity) pairs
        return pricing.calc_ttl([(price, qty) for _, price, qty in self.lines], discount)
