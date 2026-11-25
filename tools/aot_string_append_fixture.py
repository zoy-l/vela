"""AOT indexed append preserves Unicode, growth failures and native roots."""

SOURCE = r'''
external func freeze(): void = 'append.freeze';
export func calculate(operation: int): int {
  const builder = String();
  if (operation >= 3) {
    if (operation == 8) { builder.append(''[operation - 8]); }
    builder.append('A\u0000Z'[operation - 4]);
    const result = builder.toString();
    const expected = operation == 4 ? 'A' : (operation == 5 ? '\u0000' : 'Z');
    return result == expected ? 1 : 0;
  }
  if (operation == 1) {
    for (let index = 0; index < 64; index++) { builder.append('a'[0]); }
    freeze();
    builder.append('b'[0]);
    return builder.toString().length;
  }
  const text = 'A\u0000中😀e\u0301\u{10FFFF}Z';
  let expected = '';
  for (let round = 0; round < 32; round++) {
    for (let index = 0; index < text.length; index++) { builder.append(text[index]); }
    expected += text;
  }
  return builder.toString() == expected ? 1 : 0;
}
'''

HOST_TESTS = r'''
extern "C" const NativeImage* STRING_APPEND_IMAGE() noexcept;

class NativeStringAppendTest : public NativeImageTest<STRING_APPEND_IMAGE, 1> {
 protected:
  void RegisterNatives() override {
    ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<FreezeAllocations>("append.freeze")));
  }
};

TEST_F(NativeStringAppendTest, IndexedUnicodeCrossesGrowthBoundariesUnderGc) {
  NativeExecution execution(*instance);
  const std::array input{Integer(0)};
  NativeValue output;
  ASSERT_EQ(Execute(STRING_APPEND_ENTRY, execution, input, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 1);
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
}

TEST_F(NativeStringAppendTest, FailedAsciiGrowthReleasesRootsAndAllowsTheNextInvocation) {
  NativeExecution execution(*instance);
  ExpectOutOfMemoryAndRetry(STRING_APPEND_ENTRY, execution, 1, 1);
}

TEST_F(NativeStringAppendTest, LiteralAsciiPreservesNullBytesAndDynamicIndexFailures) {
  for (const auto operation : {4, 5, 6}) {
    NativeExecution execution(*instance);
    const std::array input{Integer(operation)};
    NativeValue output;
    ASSERT_EQ(Execute(STRING_APPEND_ENTRY, execution, input, output), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 1);
  }
  for (const auto operation : {3, 7, 8}) {
    NativeExecution execution(*instance);
    const std::array input{Integer(operation)};
    NativeValue output;
    EXPECT_EQ(Execute(STRING_APPEND_ENTRY, execution, input, output),
              ExecutionStatus::IndexOutOfBounds);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
  }
}
'''
