// 自检固件：内容截取自 DXVK master 的 src/dxgi/dxgi_adapter.cpp，
// 只保留 QueryVideoMemoryInfo() 与补丁锚点，用于离线验证 apply-budget-patch.py。
// 这不是 DXVK 的真实源码文件，不会被编译。

  HRESULT STDMETHODCALLTYPE DxgiAdapter::QueryVideoMemoryInfo(
          UINT                          NodeIndex,
          DXGI_MEMORY_SEGMENT_GROUP     MemorySegmentGroup,
          DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) {
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL
     && MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    DxvkAdapterMemoryInfo memInfo = m_adapter->getMemoryHeapInfo();

    VkMemoryHeapFlags heapFlagMask = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    VkMemoryHeapFlags heapFlags    = 0;

    if (MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
      heapFlags |= VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    pVideoMemoryInfo->Budget = 0;
    pVideoMemoryInfo->CurrentUsage = 0;
    pVideoMemoryInfo->AvailableForReservation = 0;

    for (uint32_t i = 0; i < memInfo.heapCount; i++) {
      if ((memInfo.heaps[i].heapFlags & heapFlagMask) != heapFlags)
        continue;

      pVideoMemoryInfo->Budget += memInfo.heaps[i].memoryBudget;
      pVideoMemoryInfo->CurrentUsage += memInfo.heaps[i].memoryAllocated;
      pVideoMemoryInfo->AvailableForReservation += memInfo.heaps[i].heapSize / 2;
    }

    // We don't implement reservation, but the observable
    // behaviour should match that of Windows drivers
    uint32_t segmentId = uint32_t(MemorySegmentGroup);

    pVideoMemoryInfo->CurrentReservation = m_memReservation[segmentId];
    return S_OK;
  }
