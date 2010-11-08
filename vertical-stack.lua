-- Vertical-Stack Mode

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
      x = 0
      y = (screen_height / master_count) * i
      width = (screen_width / 2) + margin
      height = screen_height / master_count
   else
      x = (screen_width / 2) + margin
      y = (screen_height / (a - master_count)) * ((a - 1) - i)
      width = (screen_width / 2) - margin
      height = screen_height / (a - master_count)
   end
end
