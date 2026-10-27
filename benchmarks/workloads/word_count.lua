local floor = math.floor

return function(input)
  local positions, counts = {}, {}
  for index = 0, #input - 1 do
    local line = "event_" .. tostring(index - floor(index / 64) * 64) .. ":ok"
    local word = string.sub(line, 1, string.find(line, ":", 1, true) - 1)
    local position = positions[word]
    if position == nil then
      position = #counts + 1
      positions[word], counts[position] = position, 1
    else
      counts[position] = counts[position] + 1
    end
  end
  local frequencies = {}
  for index = 1, #counts do frequencies[index] = counts[index] end
  local checksum = #counts
  for index = 1, #frequencies do
    checksum = checksum + frequencies[index] * frequencies[index]
  end
  return checksum
end
