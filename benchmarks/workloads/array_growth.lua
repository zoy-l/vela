local new = require("table.new")

return function(input, capacity)
  local values = capacity == 0 and {} or new(capacity, 0)
  for index = 1, #input do values[index] = input[index] end
  local checksum = 0
  for index = 1, #values do checksum = checksum + values[index] end
  for index = #values, 1, -1 do
    local value = values[index]
    values[index] = nil
    if value == nil or value ~= index - 1 then return -1 end
    checksum = checksum + value
  end
  return checksum
end
