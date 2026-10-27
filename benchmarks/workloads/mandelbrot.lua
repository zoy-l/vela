return function(input)
  local size = #input
  local scale = 2 / size
  local checksum = 0
  for y = 0, size - 1 do
    local imaginary = y * scale - 1
    for x = 0, size - 1 do
      local real = x * scale - 1.5
      local zr, zi, iterations = 0, 0, 0
      while iterations < 50 and zr * zr + zi * zi <= 4 do
        local next = zr * zr - zi * zi + real
        zi = 2 * zr * zi + imaginary
        zr = next
        iterations = iterations + 1
      end
      checksum = checksum + iterations
    end
  end
  return checksum
end
