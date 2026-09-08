#include "../devices/cardputer-adv/src/checked_file_print.h"
#include <cassert>
int main() {
  const uint8_t row[] = {'a', ',', 'b', '\n'};
  File complete;
  CheckedFilePrint success(complete);
  assert(success.write(row, sizeof(row)) == sizeof(row));
  assert(success.finish() && complete.flushed);
  File partial;
  partial.allowance = 2;
  CheckedFilePrint shortWrite(partial);
  assert(shortWrite.write(row, sizeof(row)) == 2);
  assert(partial.getWriteError() == 0); // Reproduces a silent positive short write.
  partial.allowance = 100;
  shortWrite.write(row, sizeof(row));
  assert(!shortWrite.finish()); // Later successful writes don't hide data loss.
  File full;
  full.allowance = 0;
  CheckedFilePrint failed(full);
  assert(failed.write(uint8_t('x')) == 0 && !failed.finish());
  File flushError;
  CheckedFilePrint lateFailure(flushError);
  lateFailure.write(row, sizeof(row));
  flushError.error = 1;
  assert(!lateFailure.finish());
}
