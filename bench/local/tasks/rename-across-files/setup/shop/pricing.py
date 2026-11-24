"""Price calculations."""

TAX_RATE = 0.08


def calc_ttl(items, discount=0.0):
    """Total price of ``items`` (list of (unit_price, quantity)) after discount and tax.

    ``discount`` is a fraction between 0 and 1 applied before tax. The result is rounded to cents.
    """
    if not 0.0 <= discount <= 1.0:
        raise ValueError("discount must be between 0 and 1")
    subtotal = sum(price * qty for price, qty in items)
    return round(subtotal * (1 - discount) * (1 + TAX_RATE), 2)
