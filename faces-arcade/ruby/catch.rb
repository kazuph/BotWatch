# 左右に動いて星を受ける。
black = Screen.rgb(14, 18, 38)
white = Screen.rgb(238, 240, 250)
yellow = Screen.rgb(255, 208, 70)
pink = Screen.rgb(255, 92, 138)
x = 145
star_x = rand(300) + 10
star_y = 0
score = 0
loop do
  x -= 5 if Pad.down?(:left)
  x += 5 if Pad.down?(:right)
  x = 0 if x < 0
  x = 290 if x > 290
  star_y += 3
  if star_y >= 205
    score += 1 if star_x >= x && star_x <= x + 30
    star_x = rand(300) + 10
    star_y = 0
  end
  Screen.clear(black)
  Screen.text("スコア #{score}", 8, 8, white)
  Screen.circle(star_x, star_y, 7, yellow)
  Screen.fill_rect(x, 210, 30, 12, pink)
  Screen.show
  sleep_ms(30)
end
