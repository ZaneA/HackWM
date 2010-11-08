-- Horizontal-Stack Mode

-- a = window_count
-- i = 0
-- margin = 0

if a == 1 then
   x = 0
   y = 0
   width = screen_width
   height = screen_height
else
   if i < master_count then
      x = (screen_width / master_count) * i
      y = 0
      width = screen_width / master_count
      height = (screen_height / 2) + margin
   else
      x = (screen_width / (a - master_count)) * ((a - 1) - i)
      y = (screen_height / 2) + margin
      width = screen_width / (a - master_count)
      height = (screen_height / 2) - margin
   end
end
