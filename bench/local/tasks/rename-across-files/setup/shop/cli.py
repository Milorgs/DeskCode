"""Demo entry point: python3 -m shop.cli"""
from shop.cart import Cart
from shop.pricing import TAX_RATE, calc_ttl


def main():
    cart = Cart()
    cart.add("pen", 1.5, 4)
    cart.add("notebook", 3.25, 2)
    print(f"tax rate: {TAX_RATE:.0%}")
    print(f"cart total: {cart.total():.2f}")
    print(f"with 10% off: {cart.total(0.1):.2f}")
    print(f"single pen: {calc_ttl([(1.5, 1)]):.2f}")


if __name__ == "__main__":
    main()
