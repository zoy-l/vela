return function(input)
  local size = #input
  local flags = {}
  for index = 1, size do flags[index] = 1 end
  local prime = 2
  while prime * prime < size do
    if flags[prime + 1] == 1 then
      local multiple = prime * prime
      while multiple < size do
        flags[multiple + 1] = 0
        multiple = multiple + prime
      end
    end
    prime = prime + 1
  end
  local count = 0
  for value = 2, size - 1 do count = count + flags[value + 1] end
  return count
end
