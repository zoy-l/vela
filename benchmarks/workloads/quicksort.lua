local floor = math.floor

local function sort(values, first, last)
  while first < last do
    local left, right = first, last
    local pivot = values[floor((first + last) / 2)]
    while left <= right do
      while values[left] < pivot do left = left + 1 end
      while values[right] > pivot do right = right - 1 end
      if left <= right then
        values[left], values[right] = values[right], values[left]
        left, right = left + 1, right - 1
      end
    end
    if right - first < last - left then
      if first < right then sort(values, first, right) end
      first = left
    else
      if left < last then sort(values, left, last) end
      last = right
    end
  end
end

return function(input)
  local values = {}
  for index = 1, #input do
    local scaled = input[index] * 73 + 19
    values[index] = scaled - floor(scaled / 65521) * 65521
  end
  sort(values, 1, #values)
  local checksum = 0
  for index = 1, #values do
    if index > 1 and values[index - 1] > values[index] then return -1 end
    checksum = checksum + values[index] * index
  end
  return checksum
end
