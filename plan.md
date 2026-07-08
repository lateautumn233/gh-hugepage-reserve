# v10 設計:`pool_want_with_cma` 雙池彈性(CMA 料場)

> 目標:VM 閒置時記憶體以 MIGRATE_CMA pageblock 形式「還給 app」;VM 開機前 acquire 從料場秒級拼回 2MB 大頁。
> 基線:本 repo v9(`gh_hugepage_reserve.c`)。`pool_want_with_cma=0`(預設)= 特性全關,行為與 v9 完全相同。

## 0. 實測依據(2026-07,兩台真機)

- 料場路徑 acquire 8GB ≈ **10 秒**(`reservoir 3843→38`);純 sweep 撞碎片牆只到 ~1290/4096(84% 的 2MB 窗有搬不動的 straggler)。
- app 確實會消費手搓 CMA:balloon/安兔兔實測 `nr_free_cma 8.19G→0`(一加 15 restrict_cma_redirect=false;聯想 cmdline 無 token 但**實測 app 一樣吃得到**——以實測為準,不以 cmdline 推斷)。
- `-f` fork(Model C′)是單向:shrink 把塊 `__free_pages` 丟回 buddy,料場不回補 → v10 要補的正是回程。
- pageblock order 依核心版本分歧:6.6/6.12=9(2MB)、6.1=10(4MB,`OPD2404` 實測)、6.18 `set_pageblock_migratetype` 已 static 化 → 特性自動關。

## 1. 模型

```
總監護目標 = pool_want_with_cma        普通池:held order-9 compound → serve VM(kretprobe 替換)
其中 held  = pool_want(既有)          CMA 料場:pageblock=MIGRATE_CMA、頁 free 在 buddy
                                        → 閒置時 app movable 分配可用
                                        → unmovable 永遠進不來 → acquire 拼裝必成
不變量:pool_want ≤ pool_want_with_cma ≤ pool_size_max
```

- serve 單位固定 order-9(2MB);migratetype 翻轉單位 = 一個 pageblock(`SUBBLKS = 1 << (pb_order − 9)` 個 2MB 子塊)。
- order-9 裝置:SUBBLKS=1,所有配對/交換機制退化為 no-op。

## 2. 參數 / sysfs

| 名稱 | 權限 | 說明 |
|---|---|---|
| `pool_want_with_cma` | 0600 | 總目標。寫入語意見 §5 |
| `migrate_cma_val` | 0400 | preflight(kapi_check)自 BTF 讀出傳入;-1=關特性 |
| `pageblock_order_val` | 0400 | preflight 讀 `/proc/pagetypeinfo`「Page block order」傳入。kernel 內無變數(純巨集,kallsyms/BTF 皆無) |
| `cma_reservoir_floor_mb` | 0600 | headroom floor,預設 1024(見 §9) |
| `pool_cma` | RO | 料場 2MB 當量(= pageblock 數 × SUBBLKS) |
| `pool_avail_cma_able` | RO | avail 中「整 pageblock 全 avail」塊數(pb-hash 全滿 entry × SUBBLKS) |
| `cma_usage` | RO | GUI 佔用統計,見 §11(commit ③ 之後) |

- 特性開啟時 `pool_want` / `pool_want_with_cma` **向上對齊 SUBBLKS 倍數**(order-9 無感)。
- `refill_stat` 追加行:`pool_want_with_cma=`、`pool_cma=`、`pool_avail_cma_able=`、`cma_pb_order=`(`=`-split,GUI 相容)。
- `reclaim_debug` 追加 `cma_leak=`(C4,恆 0)。

## 3. 狀態設計(無歷史狀態原則)

**唯一持久狀態:`cma_blocks[]`** —— 「現在是 CMA」的 pageblock 清單(stage-in 移出、翻回/建場加入、寫小拆除、exit 還原都靠它)。無 origin、無歷史 mask、無逃逸表。

**pb-hash(衍生索引,僅 SUBBLKS>1 時存在)**:

```c
struct pb_node {
    unsigned long pb;      /* key: pfn >> pb_order_rt(addr 前 n 位,同 pageblock 落同 entry)*/
    u8  avail_mask;        /* 子塊在主池 avail */
    u8  served_mask;       /* 子塊借給 VM 中 */
    u8  limbo_mask;        /* 子塊在待配池 */
    u16 next;              /* bucket chain(碰撞靠 key 精確比對,同 served_hash 模式)*/
};
/* 監護 = avail|served|limbo;完整度 = popcount(監護)
   A 類 = avail 全滿(可立即翻 CMA)
   B 類 = 監護全滿且 served≠0(等兄弟回來)
   C 類 = 監護不滿(先丟)
   翻 CMA 條件:必須全 avail(served/limbo 成員在只代表有希望) */
```

- 靜態預配陣列 + u16 freelist(抄 `served_nodes`,`:355-435`);子塊位 = `(pfn>>9) & (SUBBLKS-1)`。
- **專屬 leaf raw spinlock**(mask 更新橫跨 pool_lock/served_lock 兩域,不糾纏既有鎖)。
- 維護點(全是 bit 翻轉,atomic 合法):serve(avail→served)、hook 回收(served→avail)、hook 拒收逃逸(served→清,不設 avail)、reacquire(served→avail)、**reconcile purge(served→清,易漏!)**、pool free / 待配淘汰(清位)。三 mask 全空 → entry 回收。
- 可隨時從 pool 陣列 + 待配池 + served 表全量重建 = 衍生索引,非歷史狀態。

**待配池(limbo,= acquire stash,同一結構)**:held compound、不可 serve、**不計 avail**、上限小(~64)。出路(全在 process context 既有觸發點,無新 worker):組齊 → 整塊翻 CMA(有赤字)/ 補 avail(有席位);久組不齊 → 真 free。

## 4. kapi 新增 + 偵測驗證

- 新符號 ×2(kallsyms/kprobe 解析,登記 `kapi_abi.tsv`):
  - `set_pageblock_migratetype(struct page *, int)` —— 寫。6.18 是 static → 解析 NULL → 特性自動關。
  - `get_pfnblock_flags_mask(page*, ulong pfn, ulong mask)` —— 讀(mask=0x7)。**模組自身 inline 的 get/set 在 build≠device pageblock_order 時 bitidx 錯位 → CMA 路徑一律走 kernel 解析版**。
- `CONTIG_RANGE_CMA` shim:<6.16 第三參傳 `migrate_cma_val`;≥6.16 傳 `ACR_FLAGS_CMA`(kraw `:677` 既有結構)。
- `kapi_can_cma()`:setter && reader && contig_range && contig_pages && prep_compound && `migrate_cma_val≥0` && `pageblock_order_val∈[9..11]` && 首塊驗證通過。缺任一 → `-ENOSYS`,回退 v9 路徑。

**首塊驗證協定**(取代獨立 probe;兩倍跨度、兩倍對齊持有):

```
寫入前(非破壞): 讀 base+PB_NR−1、base+PB_NR → 兩個都必須 MOVABLE
    不過 → 未寫任何東西,釋放換一塊(bounded ~8 次;耗盡 → 特性關)
寫入:            set_pageblock_migratetype(base, migrate_cma_val)
寫入後:          base+PB_NR−1 必須 CMA(抓 pageblock_order_val 偏大)
                  base+PB_NR   必須 MOVABLE(抓偏小/翻轉溢出)
    不過 → 立即翻回、釋放、特性關 + pr_err(系統性錯,換塊無意義)
續:              free 一個 2MB → NR_FREE_CMA_PAGES 必須 +512(migrate_cma_val 語意驗證)
                  → CONTIG_RANGE_CMA 抓回 → 前半 = 料場第 1 塊;後半照常翻 = 第 2 塊(零浪費)
```

±1 級粒度錯全程安全(翻轉不出持有區);之後每塊:寫前白名單檢查(§9,不過 → skip 換塊)+ 寫後單點 readback ==CMA;邊界讀只在首塊。

## 5. 寫入語意

| 寫入 | 行為 |
|---|---|
| `with_cma` 寫大 | **只更新目標,不觸發動作**(建設一律由 acquire) |
| `with_cma` 寫小(非0) | clamp 下限 `pool_want`;超額料場**立即拆除**:`CONTIG_RANGE_CMA` 抓回 → 翻 MOVABLE → free → 除名。最空的塊先拆(抓取便宜) |
| `with_cma` 寫 0 | 停用:全料場拆除;偵測/驗證結果保留(再啟用不重測) |
| `pool_want` 寫入 > with_cma | **with_cma 自動跟到同值**(舊管理 app 相容)——僅當 with_cma>0;0 是停用哨兵,不被拉起 |
| `pool_want` 調小 | shrink 三類推導(§7) |
| rmmod | 料場全還原(CMA 抓回→翻 MOVABLE→free)→ 再 free pool |

舊 app 只寫 `pool_want` 即可驅動完整彈性循環(調大→with_cma 跟上→acquire 填;調小→shrink 翻回料場)。

## 6. acquire

**外層停止條件:`served + avail + pool_cma ≥ pool_want_with_cma`**;填充順序 pool 先、料場後。

```
Phase 1/2(填 pool 到 pool_want,照舊架構):
    來源順序:總監護 ≥ with_cma → 料場 stage-in 優先;不足 → 外部優先
    配對優先:掃 pb-hash 半滿 entry,targeted CONTIG_RANGE 補缺席 sibling;
              grab_free 拿到的塊入表後立刻試抓 sibling(趁 buddy 相鄰也空)
Phase R(填料場):sweep 續掃,每拼出一個 2MB:
    湊得齊整 pageblock → 整塊直接翻 CMA 入料場(不過 pool)
    湊不齊(sibling 是 straggler)→ 入待配池
    待配滿 / 掃畢 → 交換:pool 摘一組 cma_able(SUBBLKS 塊)翻 CMA,待配補散塊回填席位(1:1)
    待配收納上限 = 當下 pool 內 cma_able 組數 × SUBBLKS(先算再收);剩餘 free
Phase Q(品質模式):數量達標但 pool 內 non-cma-able > 0 亦可觸發/繼續:
    targeted CONTIG_RANGE 補缺 sibling → 成功則真 free 一塊最低完整度散塊換入
    (process context 可真丟);順手處理待配池 promote/淘汰
    無進展 → 停,acquire_stop_reason = "quality converged"
```

**Stage-in 以整 pageblock 為單位**:`CONTIG_RANGE_CMA(base, base+PB_NR)` 整塊抓(遷走 app squatter,movable 必遷得動)→ 驗讀 ==CMA → 翻 MOVABLE → 拆 SUBBLKS 個 order-9 `rebuild_order9_compound` 全入 pool(暫超 want < SUBBLKS 容忍,free-hook gate 自然消化)→ `cma_blocks[]` 除名。

停止原因擴充:`reached target(with_cma)` / `cma sources exhausted` / `quality converged` / floor / 使用者中斷。`acquire_set` 開頭「已達標」檢查同步改用新條件。

## 7. shrink(`pool_want` 調小,`pool_do_resize` `:1753`)

```
按 pageblock 分組(pb-hash 一次 lookup 分類):
  A. avail 全滿+對齊+白名單 → 先 set CMA 再逐塊 free(落 CMA freelist)→ 入料場
       cap = with_cma − want;每塊吃 headroom floor
  B. 監護全滿但有 served → avail 成員最後丟(等兄弟回來變 A)
  C. 監護不滿 → 先丟 buddy(照今天 __free_pages)
```

## 8. free-hook 回收路徑

**主流程零改動**:order gate → `served_del` → `pool_take_frozen`(gate `< pool_want` 不變)→ bypass。VM 還頁一律回普通池;翻 CMA 只發生在 §5/§7 的 process context。

- **C4 tripwire(pool 全部入口:take_frozen / push / push_grow / 待配)**:kernel reader 讀 pageblock 型別 ==CMA → 拒收(頁流向 buddy 落 CMA freelist = 正確歸宿)+ `cma_leak++` + `pr_err_ratelimited`。正常恆 0(B 類保護保證);非 0 = 邏輯 bug 警報。不用 VM_WARN(產線 DEBUG_VM 關)、不 panic。
- **order-10 交換路徑(僅 SUBBLKS>1 且 gate 拒收時,即 want 中途調小的 corner)**:

```
pb-hash 一次 lookup:sibling 在監護中(avail|served|limbo)?
  ├─ 在 → 收進主池 + 降級一塊低完整度塊到待配池(victim 搜尋有界;找不到 → 新塊自己入待配)
  └─ 不在 / 待配滿 → 照今天 drain(逃逸:served bit 清、不設 avail;Phase R/Q 重建)
全程 list/bit 操作;atomic 內無 free、無翻轉
```

- 常態(gate 過)路徑零額外開銷;完整度優化交給 Phase Q。
- `pcp_drain_worker` / `served_reacquire_free_orphans`(`:1005`)照舊;入口同被 C4 覆蓋。

## 9. 不變量 / 護欄

| 條目 | 內容 |
|---|---|
| 席位保留 | `avail + served ≤ pool_want`,所有外部填充者遵守。acquire 既有條件已符(`:2152`);**必修 `refill_worker`(`:1456`)**:`target = pool_total − avail` 未扣 served = v9 置換引擎,VM 回收會被外部塊擠掉 |
| 總量 | `avail + served + 待配 + pool_cma ≤ pool_want_with_cma`;待配 ≤ cap |
| 翻轉 context | migratetype 翻轉一律 process context;atomic(hook)永不翻、永不 free |
| 翻 CMA 白名單 | 讀整塊 label:接受 {UNMOVABLE(0), MOVABLE(1), RECLAIMABLE(2)}(grab_free 常帶 fallback-steal 的 UNMOVABLE 殘標);拒 CMA(3,別人的)/ HIGHATOMIC(4,`nr_reserved_highatomic` 帳本)/ ISOLATE(5)/ ≥6(CHP ext,vendor `CHP_BUG_ON` panic)。首塊驗證用嚴格 MOVABLE |
| headroom floor | 每翻一塊 CMA 前:`(si_mem_available − NR_FREE_CMA_PAGES) > cma_reservoir_floor_mb`——CMA 只給 movable,翻過頭餓死 kernel unmovable 預算(裝置常駐 ~3.5G 不可移動)。與 `acquire_mem_floor_mb`(sweep 防 reclaim livelock)是不同的煞車,並存 |
| serve | 不挑 label(pin 只拒 CMA/ISOLATE);push 成組壓棧底、散塊放棧頂(LIFO 先發散塊,近似 ext-first,不養狀態) |
| 逃逸 | 不追;Phase R/Q 重建(用重建取代取證) |

## 10. init / exit / 不動清單

- **init**(`:2539`):kapi_init → 對齊兩個 want → **先建料場**(記憶體最乾淨;首塊 = 驗證;floor 護欄;`pr_warn` 停點)→ 預填 pool(照舊 `alloc_pages`)。
- **exit**(`:2715`):料場全還原 → 既有 teardown 順序 → free pool。
- **不動**:serve kretprobe(`:1177`)、served 表、owner 追蹤、reconcile 主流程、free-hook 主流程、sweep 本體(`:2311`)、`block_candidate`(`:2209`,整-zone 掃描仍拒 CMA——vendor carveout 不碰,自家料場走 `cma_blocks[]` 專屬路徑)。

## 11. GUI 觀測:`cma_usage`(RO,commit ③ 後)

模組是唯一知道料場範圍的人(未註冊 `struct cma`,debugfs 看不到;全域 CmaFree 混 vendor)。process context 掃 `cma_blocks[]`,buddy order / folio_nr_pages 跳步 + cond_resched,內部 rate-limit(~1s 快取):

```
reservoir_mb= / free_mb= / used_mb= / used_anon_mb= / used_file_mb=
blocks_free= blocks_partial= blocks_full=
```

`used_anon_mb` 兼作 acquire 成本預估(開 VM 前要遷多少)。快照 racy,GUI 用途可接受。

## 12. Commit 切分

| # | 內容 | 驗證方法 |
|---|---|---|
| ① | kapi 兩符號 + 參數骨架 + 首塊驗證 + 料場 build/teardown + §5 寫入語意 + C4 tripwire | 兩台真機:載入建場 → NR_FREE_CMA 對帳 → 寫小拆除 → 卸載還原;OPD2404 驗 order-10 首塊協定;6.18 tag 驗自動關 |
| ② | acquire:stage-in + 配對 + Phase R(stash 交換)+ Phase Q + 新停止條件;pb-hash + 待配池結構 | 安兔兔壓測後 acquire,對照 -f fork 的 10 秒拿滿基線 |
| ③ | shrink A/B/C + `refill_worker` 席位修正 + hook 交換路徑 + `cma_usage` | 降 want → `pool_cma` 回升(對照 -f 單向);kill VM → `cma_leak` 恆 0、回收 100% |

每個 commit 過既有 symbol-safety 檢查(7 tag build + `nm -u` diff 不得增長)。

## 13. 機隊現實

| 裝置 | 核心 | pb order | 路徑 |
|---|---|---|---|
| 一加 15(SM8850,KernelSU) | 6.12 | 9 | 主路徑;SUBBLKS=1 全退化;app 吃 CMA 實測成立 |
| 聯想 TB322FC(SM8750P,Magisk) | 6.6 | 9 | 同上(restrict 推斷失效,以實測為準) |
| OPD2404(SM8650,pineapple) | 6.1 | **10** | SUBBLKS=2 全套(pb-hash/待配/交換);首塊驗證必測機 |
| (未來)6.18 | 6.18 | — | setter static → 特性自動關,v9 路徑 |
