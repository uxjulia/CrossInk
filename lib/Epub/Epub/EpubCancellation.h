#pragma once

// Optional, borrowed for one EPUB operation. A cancellation stays latched
// until the book and its temporary writers have closed.
struct EpubCancellation {
  bool (*shouldCancel)(void*) = nullptr;
  void* cancelContext = nullptr;
  bool isCancellationRequested() const { return shouldCancel && shouldCancel(cancelContext); }
};
