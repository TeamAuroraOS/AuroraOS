import aurora


def main():
    x = 200
    y = 120
    while aurora.loop():
        if aurora.keys_down():
            break
        aurora.clear(aurora.SCREEN_TOP, aurora.COLOR_BLACK)
        aurora.circle(aurora.SCREEN_TOP, x, y, 12, aurora.COLOR_AURORA)
