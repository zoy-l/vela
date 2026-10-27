local floor = math.floor

local function accumulator(start)
  local current = start
  return function(value)
    local sum = current + value
    current = sum - floor(sum / 65521) * 65521
    return current
  end
end

return function(input)
  local next = accumulator(7)
  local mapped = {}
  for index = 1, #input do mapped[index] = next(input[index]) end
  local total = 0
  for index = 1, #mapped do total = total + mapped[index] end
  return total
end
