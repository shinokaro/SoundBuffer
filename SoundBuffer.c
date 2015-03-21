/*
 * Copyright (c) <2015> <shinokaro>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *    1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 *
 *    2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 *
 *    3. This notice may not be removed or altered from any source
 *    distribution.
 */
/*
 * このコードはmirichi氏の書いたコードを元にshinokaroが作成した。
 * RubyからDirectSoundの利用方を示してくれたmirichi氏に感謝する。
 */
#include "ruby.h"
/*
 * DirectSoundではGUIDを引数に使用することがある。
 * GUID_NULLの定義が必要になる。このためks.hファイルとlibuuidをリンクする必要がある。
 */
#include "ks.h"
/*
 * MSから配布されている開発キットをインストールしてdsound.hとsal.hを入手する必要がある。
 * MinGW付属のdsound.hはDirectXで使用できる全ての機能をサポートしていない。
 * また、MinGWが提供するdsound.hはWineプロジェクトのものであり、そのライセンスはGPLである。
 *
 * MSのsdound.hはsal.hを内部でインクルードしている。
 * このままコンパイルすると__nullが定義されないためエラーが出る。
 * そのため__nullを自前で定義する必要がある。
 */
#define __null
#include <dsound.h>
#include <string.h>

// Ruby側のエフェクト指定用の定数
// クラス定義のセクションでRuby定数定義を行っている。
#define FX_GARGLE       0
#define FX_CHORUS       1
#define FX_FLANGER      2
#define FX_ECHO         3
#define FX_DISTORTION   4
#define FX_COMPRESSOR   5
#define FX_PARAM_EQ     6
#define FX_I3DL2_REVERB 7
#define FX_WAVES_REVERB 8

// RubyのSoundTestクラス
static VALUE cSoundBuffer;

// Rubyの例外オブジェクト
static VALUE eSoundBufferError;

// COMのDirectSoundオブジェクト
static LPDIRECTSOUND8 g_pDSound;

// RubyのSoundTestオブジェクトが持つC構造体
struct SoundBuffer {
  LPDIRECTSOUNDBUFFER8  pDSBuffer8;
  size_t                buffer_bytes;
  WORD                  channels;
  DWORD                 samples_per_sec;
  WORD                  bits_per_sample;
  WORD                  block_align;
  DWORD                 avg_bytes_per_sec;
  DWORD                 ctrlfx_flag;
  VALUE                 effect_list;
  long                  play_flag;
  long                  repeat_flag;
};

// COM終了条件はshutdown+すべてのSoundTestの解放なのでカウントする
static int g_refcount = 0;

// プロトタイプ宣言
static void   SoundBuffer_mark(void *s);
static void   SoundBuffer_free(void *s);
static size_t SoundBuffer_memsize(const void *s);

// TypedData用の型データ
const rb_data_type_t SoundBuffer_data_type = {
  "SoundBuffer",
  {
    SoundBuffer_mark,    // マーク関数
    SoundBuffer_free,    // 解放関数
    SoundBuffer_memsize, // サイズ関数
  },
  NULL,
  NULL
};

// GCのマークで呼ばれるマーク関数
static void
SoundBuffer_mark(void *s)
{
  struct SoundBuffer *st = (struct SoundBuffer *)s;
  // need dispose check
  // マーク
  rb_gc_mark(st->effect_list);
}

// DirectSoundバッファを開放する内部用関数
static void
SoundBuffer_release(struct SoundBuffer *st)
{
  if (st->pDSBuffer8) {
    st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
    st->pDSBuffer8->lpVtbl->Release(st->pDSBuffer8);
    st->pDSBuffer8  = NULL;
    st->effect_list = Qnil;

    // shutduwn+すべてのSoundTestが解放されたらDirectSound解放
    g_refcount--;
    if (g_refcount == 0) {
      g_pDSound->lpVtbl->Release(g_pDSound);
      CoUninitialize();
    }
  }
}

// GCで回収されたときに呼ばれる解放関数
static void
SoundBuffer_free(void *s)
{
  // バッファ解放
  SoundBuffer_release((struct SoundBuffer *)s);

  // SoundTest解放
  xfree(s);
}

// ObjectSpaceからのサイズの問い合わせに応答する
static size_t
SoundBuffer_memsize(const void *s)
{
  struct SoundBuffer *st = (struct SoundBuffer *)s;

  // ざっくりstruct SoundBufferとバッファのサイズを足して返す
  return sizeof(struct SoundBuffer) + st->buffer_bytes;
  // Dup対応のため、クラス側にメモリーリストを導入する。
}

// SoundTest.newするとまずこれが呼ばれ、次にinitializeが呼ばれる
static VALUE
SoundBuffer_allocate(VALUE klass)
{
  VALUE obj;
  struct SoundBuffer *st;

  // RubyのTypedData型オブジェクトを生成する
  obj = TypedData_Make_Struct(klass, struct SoundBuffer, &SoundBuffer_data_type, st);

  // allocate時点ではバッファサイズが不明なのでバッファは作らない
  st->pDSBuffer8        = NULL;
  st->buffer_bytes      = 0;
  st->channels          = 0;
  st->samples_per_sec   = 0;
  st->bits_per_sample   = 0;
  st->block_align       = 0;
  st->avg_bytes_per_sec = 0;
  st->ctrlfx_flag       = 0;
  st->effect_list       = rb_obj_freeze(rb_ary_new());
  st->play_flag         = 0;
  st->repeat_flag       = 0;
  return obj;
}

/*
 *
 */
static struct SoundBuffer *
get_st(VALUE self)
{
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);
  if (!st->pDSBuffer8) rb_raise(eSoundBufferError, "disposed object");
  return st;
}

static void
to_raise_an_exception(HRESULT hr)
{ //http://clalance.blogspot.jp/2011/01/writing-ruby-extensions-in-c-part-5.html
  switch (hr) {
    //case DS_OK:
    //  rb_raise(eSoundBufferError, "DS_OK");
    //  break;
    case DS_NO_VIRTUALIZATION:
      rb_raise(eSoundBufferError, "DS_NO_VIRTUALIZATION");
      break;
    //case DS_INCOMPLETE:
    //  rb_raise(eSoundBufferError, "DS_INCOMPLETE");
    //  break;
    case DSERR_ACCESSDENIED:
      rb_raise(eSoundBufferError, "DSERR_ACCESSDENIED");
      break;
    case DSERR_ALLOCATED:
      rb_raise(eSoundBufferError, "DSERR_ALLOCATED");
      break;
    case DSERR_ALREADYINITIALIZED:
      rb_raise(eSoundBufferError, "DSERR_ALREADYINITIALIZED");
      break;
    case DSERR_BADFORMAT:
      rb_raise(eSoundBufferError, "DSERR_BADFORMAT");
      break;
    case DSERR_BADSENDBUFFERGUID:
      rb_raise(eSoundBufferError, "DSERR_BADSENDBUFFERGUID");
      break;
    case DSERR_BUFFERLOST:
      /*
       * LOCALSOFTWAREに置かれたバッファーがロストするか不明
       * このエラーを報告するようにしてある。これが発生したら報告してほしい。
       */
      rb_raise(eSoundBufferError, "DSERR_BUFFERLOST");
      break;
    case DSERR_BUFFERTOOSMALL:
      rb_raise(eSoundBufferError, "DSERR_BUFFERTOOSMALL");
      break;
    case DSERR_CONTROLUNAVAIL:
      rb_raise(eSoundBufferError, "DSERR_CONTROLUNAVAIL");
      break;
    case DSERR_DS8_REQUIRED:
      rb_raise(eSoundBufferError, "DSERR_DS8_REQUIRED");
      break;
    case DSERR_FXUNAVAILABLE:
      rb_raise(eSoundBufferError, "DSERR_FXUNAVAILABLE");
      break;
    case DSERR_GENERIC:
      rb_raise(eSoundBufferError, "DSERR_GENERIC");
      break;
    case DSERR_INVALIDCALL:
      rb_raise(eSoundBufferError, "DSERR_INVALIDCALL");
      break;
    case DSERR_INVALIDPARAM:
      rb_raise(eSoundBufferError, "DSERR_INVALIDPARAM");
      break;
    case DSERR_NOAGGREGATION:
      rb_raise(eSoundBufferError, "DSERR_NOAGGREGATION");
      break;
    case DSERR_NODRIVER:
      rb_raise(eSoundBufferError, "DSERR_NODRIVER");
      break;
    case DSERR_NOINTERFACE:
      rb_raise(eSoundBufferError, "DSERR_NOINTERFACE");
      break;
    case DSERR_OBJECTNOTFOUND:
      rb_raise(eSoundBufferError, "DSERR_OBJECTNOTFOUND");
      break;
    case DSERR_OTHERAPPHASPRIO:
      rb_raise(eSoundBufferError, "DSERR_OTHERAPPHASPRIO");
      break;
    case DSERR_OUTOFMEMORY:
      rb_raise(eSoundBufferError, "DSERR_OUTOFMEMORY");
      break;
    case DSERR_PRIOLEVELNEEDED:
      rb_raise(eSoundBufferError, "DSERR_PRIOLEVELNEEDED");
      break;
    case DSERR_SENDLOOP:
      rb_raise(eSoundBufferError, "DSERR_SENDLOOP");
      break;
    case DSERR_UNINITIALIZED:
      rb_raise(eSoundBufferError, "DSERR_UNINITIALIZED");
      break;
    case DSERR_UNSUPPORTED:
      rb_raise(eSoundBufferError, "DSERR_UNSUPPORTED");
      break;
    case CO_E_NOTINITIALIZED:
      rb_raise(eSoundBufferError, "CO_E_NOTINITIALIZED");
      break;
    default:
      rb_raise(eSoundBufferError, "DirectSound API error"); // hrのHEX表示をつける
      break;
  }
}

/*
 *
 */
static VALUE
SoundBuffer_play_pos_eql(VALUE self, VALUE voffset)
{
  HRESULT   hr;
  DWORD     dwNewPosition = NUM2UINT(voffset);
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetCurrentPosition(st->pDSBuffer8, dwNewPosition);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return UINT2NUM(0);
}

/*
 *
 */
static VALUE
SoundBuffer_play_pos(VALUE self)
{
  HRESULT   hr;
  DWORD     dwCurrentPlayCursor;
  DWORD     dwCurrentWriteCursor;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetCurrentPosition(st->pDSBuffer8, &dwCurrentPlayCursor, &dwCurrentWriteCursor);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return UINT2NUM(dwCurrentPlayCursor);
}

/*
 *
 */
static VALUE
SoundBuffer_size(VALUE self)
{
  return UINT2NUM(get_st(self)->buffer_bytes);
}

/*
 * call-seq:
 *    sb.write(str) ->  fixnum
 *    sb.write(str, offset) ->  fixnum
 */
static VALUE
SoundBuffer_write(int argc, VALUE *argv, VALUE self)
{
  LPVOID   ptr1,  ptr2;
  DWORD    size1, size2;
  DWORD    bytes, offset, write_size;
  char    *strptr;
  HRESULT  hr;
  VALUE    vbuffer, voffset;
  struct SoundBuffer *st = get_st(self);

  rb_scan_args(argc, argv, "11", &vbuffer, &voffset);

  Check_Type(vbuffer, T_STRING);
  bytes  = RSTRING_LEN(vbuffer);
  strptr = RSTRING_PTR(vbuffer);
  offset = NIL_P(voffset) ? 0 : NUM2UINT(voffset);
  if (bytes  > st->buffer_bytes)         rb_raise(rb_eRangeError, "string length");
  if (offset > st->buffer_bytes)         rb_raise(rb_eRangeError, "offset");
  //if (offset < 0)                       rb_raise(rb_eRangeError, "negative offset");
  if (offset + bytes > st->buffer_bytes) rb_raise(rb_eRangeError, "this method is nolap mode");
  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, offset, 0, &ptr1, &size1, &ptr2, &size2, DSBLOCK_ENTIREBUFFER);
  if (FAILED(hr)) to_raise_an_exception(hr);

  write_size = bytes < size1 ? bytes : size1;
  memcpy(ptr1, strptr, write_size);

  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, write_size, ptr2, 0);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "Unlock error");

  return UINT2NUM(write_size);
}

static VALUE
SoundBuffer_write_sync(VALUE self, VALUE vbuffer)
{
  LPVOID   ptr1,  ptr2;
  DWORD    size1, size2;
  DWORD    bytes, write_size1, write_size2;
  char    *strptr;
  HRESULT  hr;
  struct SoundBuffer *st = get_st(self);

  Check_Type(vbuffer, T_STRING);
  bytes  = RSTRING_LEN(vbuffer);
  strptr = RSTRING_PTR(vbuffer);

  if (bytes  > st->buffer_bytes) rb_raise(rb_eRangeError, "string length");
  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, 0, bytes, &ptr1, &size1, &ptr2, &size2, DSBLOCK_FROMWRITECURSOR);
  if (FAILED(hr)) to_raise_an_exception(hr);

  write_size1 = bytes < size1 ? bytes : size1;
  memcpy(ptr1, strptr, write_size1);
  if (ptr2 == NULL) {
    write_size2 = 0;
  }
  else {
    write_size2 = bytes - write_size1 < size2 ? bytes - write_size1 : size2;
    memcpy(ptr2, strptr + write_size1, write_size2);
  }
  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, write_size1, ptr2, write_size2);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "Unlock error");

  return UINT2NUM(write_size1 + write_size2);
}

/*
 *
 */
static VALUE
SoundBuffer_to_s(VALUE self)
{
  LPVOID   ptr1,  ptr2;
  DWORD    size1, size2;
  HRESULT  hr;
  VALUE    str;
  struct SoundBuffer *st = get_st(self);

// 再生中にto_sできるようにするか？
  if (st->play_flag) rb_raise(eSoundBufferError, "now playing, plz stop");
  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, 0, 0, &ptr1, &size1, &ptr2, &size2, DSBLOCK_ENTIREBUFFER);
  if (FAILED(hr)) to_raise_an_exception(hr);
  if (size1 != st->buffer_bytes) rb_raise(eSoundBufferError, "can not full lock");
  str = rb_str_new(ptr1, size1); // RB_GC_GUARD必要？
  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, 0, ptr2, 0);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return str;
}

/*
 * SoundTest#initialize
 */
static VALUE
SoundBuffer_initialize(int argc, VALUE *argv, VALUE self)
{
  DSBUFFERDESC desc;
  WAVEFORMATEX pcmwf;
  LPDIRECTSOUNDBUFFER pDSBuffer;
  HRESULT hr;
  VALUE   vbuffer, vsamples_per_sec, vbits_per_sample, vchannels, vopt;
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);

  rb_scan_args(argc, argv, "13:", &vbuffer, &vchannels, &vsamples_per_sec, &vbits_per_sample, &vopt);
  switch (TYPE(vbuffer)) {
  case T_FIXNUM:
    st->buffer_bytes = NUM2UINT(vbuffer);
    break;
  case T_STRING:
    st->buffer_bytes = RSTRING_LEN(vbuffer);
    break;
  default:
    rb_raise(rb_eTypeError, "not valid value");
    break;
  }
  if (st->buffer_bytes < DSBSIZE_MIN || DSBSIZE_MAX < st->buffer_bytes) rb_raise(rb_eRangeError, "buffer size error");

  st->channels        = NIL_P(vchannels)        ? 1     : (WORD)NUM2UINT(vchannels);
  if (st->channels != 1 && st->channels != 2) rb_raise(rb_eRangeError, "channels arguments 1 and 2 only possible");

  st->samples_per_sec = NIL_P(vsamples_per_sec) ? 48000 :       NUM2UINT(vsamples_per_sec);
  if (st->samples_per_sec < DSBFREQUENCY_MIN || DSBFREQUENCY_MAX < st->samples_per_sec) rb_raise(rb_eRangeError, "samples_per_sec argument can be only DSBFREQUENCY_MIN-DSBFREQUENCY_MAX");

  st->bits_per_sample = NIL_P(vbits_per_sample) ? 16    : (WORD)NUM2UINT(vbits_per_sample);
  if (st->bits_per_sample != 8 && st->bits_per_sample != 16) rb_raise(rb_eRangeError, "bits_per_sample arguments 8 and 16 only possible");

  st->ctrlfx_flag = !NIL_P(vopt) && RTEST(rb_hash_aref(vopt, ID2SYM(rb_intern("effect")))) ? DSBCAPS_CTRLFX : 0;
  if (st->ctrlfx_flag && st->bits_per_sample == 8) rb_raise(rb_eRangeError, "do not use FX, when bits per sample is 8 bit");
  // 切捨て判定でOKか、あとで調べる。
  if (st->ctrlfx_flag && st->buffer_bytes < st->samples_per_sec * DSBSIZE_FX_MIN / 1000) rb_raise(rb_eRangeError, "buffer is small, when use FX");

  st->block_align       = st->channels * st->bits_per_sample / 8;
  st->avg_bytes_per_sec = st->samples_per_sec * st->block_align;
  // フォーマット設定
  pcmwf.wFormatTag      = WAVE_FORMAT_PCM;
  pcmwf.nChannels       = st->channels;
  pcmwf.nSamplesPerSec  = st->samples_per_sec;
  pcmwf.nAvgBytesPerSec = st->avg_bytes_per_sec;
  pcmwf.nBlockAlign     = st->block_align;
  pcmwf.wBitsPerSample  = st->bits_per_sample;
  pcmwf.cbSize          = 0;
  // DirectSoundバッファ設定
  desc.dwSize           = sizeof(desc);
  desc.dwFlags          = DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | st->ctrlfx_flag
                        | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2
                        | DSBCAPS_GLOBALFOCUS;// | DSBCAPS_STICKYFOCUS;
  desc.dwBufferBytes    = st->buffer_bytes;
  desc.dwReserved       = 0;
  desc.lpwfxFormat      = &pcmwf;
  desc.guid3DAlgorithm  = DS3DALG_DEFAULT;

  // DirectSoundバッファ生成
  hr = g_pDSound->lpVtbl->CreateSoundBuffer(g_pDSound, &desc, &pDSBuffer, NULL);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "CreateSoundBuffer error");
  hr = pDSBuffer->lpVtbl->QueryInterface(pDSBuffer, &IID_IDirectSoundBuffer8, (void**)&(st->pDSBuffer8));
  if (FAILED(hr)) rb_raise(eSoundBufferError, "QueryInterface error");
  pDSBuffer->lpVtbl->Release(pDSBuffer);

  st->ctrlfx_flag       = st->ctrlfx_flag ? 1 : 0;
  g_refcount++;

  if (TYPE(vbuffer) == T_STRING) SoundBuffer_write(1, &vbuffer, self);

  return self;
}

/*
 *
 */
static VALUE
SoundBuffer_duplicate(VALUE self)
{
  HRESULT hr;
  VALUE   obj;
  struct SoundBuffer *src, *dst;

  src = get_st(self);
  obj = SoundBuffer_allocate(cSoundBuffer);
  dst = (struct SoundBuffer *)RTYPEDDATA_DATA(obj);

  hr = g_pDSound->lpVtbl->DuplicateSoundBuffer(g_pDSound, (LPDIRECTSOUNDBUFFER)src->pDSBuffer8, (LPDIRECTSOUNDBUFFER *)&dst->pDSBuffer8);
  if (FAILED(hr)) to_raise_an_exception(hr);

  dst = src;
  g_refcount++;
  return obj;
}

/*
 *
 */
static VALUE
SoundBuffer_dispose(VALUE self)
{
  SoundBuffer_release(get_st(self));
  return self;
}

static VALUE
SoundBuffer_disposed(VALUE self)
{
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);
  return st->pDSBuffer8 ? Qfalse : Qtrue;
}

/*
 *
 */
static VALUE
SoundBuffer_get_channels(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->channels));
}

static VALUE
SoundBuffer_get_samples_per_sec(VALUE self)
{
  return UINT2NUM(get_st(self)->samples_per_sec);
}

static VALUE
SoundBuffer_get_bits_per_sample(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->bits_per_sample));
}

static VALUE
SoundBuffer_get_block_align(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->block_align));
}

static VALUE
SoundBuffer_get_avg_bytes_per_sec(VALUE self)
{
  return UINT2NUM(get_st(self)->avg_bytes_per_sec);
}

static VALUE
SoundBuffer_get_effectable(VALUE self)
{
  return get_st(self)->ctrlfx_flag ? Qtrue : Qfalse;
}
/*
 * エフェクトパラメーター反映のためのメソッド
 * エフェクトのパラメーターは再生中に変更しても反映されない。
 * ここでは何もしないLock, Unlockを行うことで再生中にエフェクト・パラメーターの変更を強制的に反映させる。
 */
static VALUE
SoundBuffer_flash(VALUE self)
{
  HRESULT hr;
  LPVOID  ptr1,  ptr2;
  DWORD   size1, size2;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, 0, 4, &ptr1, &size1, &ptr2, &size2, DSBLOCK_FROMWRITECURSOR);
  if (FAILED(hr)) to_raise_an_exception(hr);
  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, 0, ptr2, 0);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "unlock error");
  return self;
}

/*
 * #play
 */
static VALUE
SoundBuffer_play(VALUE self)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Play(st->pDSBuffer8, 0, 0, 0);
  if (FAILED(hr)) to_raise_an_exception(hr);
  st->play_flag   = 1;
  st->repeat_flag = 0;
  return self;
}

/*
 * #repeat
 */
static VALUE
SoundBuffer_repeat(VALUE self)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Play(st->pDSBuffer8, 0, 0, DSBPLAY_LOOPING);
  if (FAILED(hr)) to_raise_an_exception(hr);
  st->play_flag   = 1;
  st->repeat_flag = 1;
  return self;
}

/*
 * #stop
 */
static VALUE
SoundBuffer_stop(VALUE self)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
  if (FAILED(hr)) to_raise_an_exception(hr);
  hr = st->pDSBuffer8->lpVtbl->SetCurrentPosition(st->pDSBuffer8, 0);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetCurrentPosition error");
  st->play_flag   = 0;
  st->repeat_flag = 0;
  return self;
}

/*
 * #pause
*/
static VALUE
SoundBuffer_pause(VALUE self)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
  if (FAILED(hr)) to_raise_an_exception(hr);
  st->play_flag   = 0;
  return self;
}

static VALUE
SoundBuffer_pausing(VALUE self)
{
  HRESULT   hr;
  DWORD     dwCurrentPlayCursor;
  DWORD     dwCurrentWriteCursor;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetCurrentPosition(st->pDSBuffer8, &dwCurrentPlayCursor, &dwCurrentWriteCursor);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return !st->play_flag && dwCurrentPlayCursor > 0 ? Qtrue : Qfalse;
}

/*
 * #playing?
 */
static VALUE
SoundBuffer_playing(VALUE self)
{
  return get_st(self)->play_flag ? Qtrue : Qfalse;
}

static VALUE
SoundBuffer_repeating(VALUE self)
{
  return get_st(self)->repeat_flag ? Qtrue : Qfalse;
}

/*
 * SoundBuffer#get_volume
 */
static VALUE
SoundBuffer_get_volume(VALUE self)
{
  HRESULT hr;
  long    volume;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetVolume(st->pDSBuffer8, &volume);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return INT2NUM(volume);
}
/*
 * SoundBuffer#set_volume
 */
static VALUE
SoundBuffer_set_volume(VALUE self, VALUE vvolume)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetVolume(st->pDSBuffer8, NUM2INT(vvolume));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vvolume;
}
/*
 * SoundBuffer#get_pan
 */
static VALUE
SoundBuffer_get_pan(VALUE self)
{
  HRESULT hr;
  long    pan;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetPan(st->pDSBuffer8, &pan);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return INT2NUM(pan);
}
/*
 * SoundBuffer#set_pan
 */
static VALUE
SoundBuffer_set_pan(VALUE self, VALUE vpan)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetPan(st->pDSBuffer8, NUM2INT(vpan));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vpan;
}
/*
 * SoundBuffer#get_frequency
 */
static VALUE
SoundBuffer_get_frequency(VALUE self)
{
  HRESULT hr;
  unsigned long frequency;

  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetFrequency(st->pDSBuffer8, &frequency);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return UINT2NUM(frequency);
}
/*
 * SoundBuffer#set_frequency
 */
static VALUE
SoundBuffer_set_frequency(VALUE self, VALUE vfrequency)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetFrequency(st->pDSBuffer8, NUM2UINT(vfrequency));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vfrequency;
}
/*
 *
 */
static VALUE
SoundBuffer_get_effect(VALUE self)
{
  struct SoundBuffer *st = get_st(self);
  if (!st->ctrlfx_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  return st->effect_list;
}
/*
 * set_effect
 * 再生中のエフェクトリスト変更はエラーになる
 */
static VALUE
SoundBuffer_set_effect(VALUE self, VALUE vargs)
{
  HRESULT           hr;
  unsigned long     count;
  unsigned long     i;
  GUID              guid;
  LPDSEFFECTDESC    pDSFXDesc;
  struct SoundBuffer *st = get_st(self);

  if (!st->ctrlfx_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  Check_Type(vargs, T_ARRAY);

  // エフェクトリストのサイズ取得
  count = RARRAY_LEN(vargs);
  if (count > 0) {
    pDSFXDesc = (DSEFFECTDESC *)malloc(sizeof(DSEFFECTDESC) * count);

    for (i = 0; i < count; i++) {
      switch (NUM2INT(rb_ary_entry(vargs, i))) {
        case FX_GARGLE:
          guid = GUID_DSFX_STANDARD_GARGLE;
          break;
        case FX_CHORUS:
          guid = GUID_DSFX_STANDARD_CHORUS;
          break;
        case FX_FLANGER:
          guid = GUID_DSFX_STANDARD_FLANGER;
          break;
        case FX_ECHO:
          guid = GUID_DSFX_STANDARD_ECHO;
          break;
        case FX_DISTORTION:
          guid = GUID_DSFX_STANDARD_DISTORTION;
          break;
        case FX_COMPRESSOR:
          guid = GUID_DSFX_STANDARD_COMPRESSOR;
          break;
        case FX_PARAM_EQ:
          guid = GUID_DSFX_STANDARD_PARAMEQ;
          break;
        case FX_I3DL2_REVERB:
          guid = GUID_DSFX_STANDARD_I3DL2REVERB;
          break;
        case FX_WAVES_REVERB:
          guid = GUID_DSFX_WAVES_REVERB;
          break;
        default:
          free(pDSFXDesc);
          rb_raise(rb_eTypeError, "not valid value");
          break;
      }
      pDSFXDesc[i].dwSize        = sizeof(DSEFFECTDESC);
      pDSFXDesc[i].dwFlags       = DSFX_LOCSOFTWARE;      // dwFlagは強制的にソフトウェアー配置になるよう設定
      pDSFXDesc[i].guidDSFXClass = guid;
      pDSFXDesc[i].dwReserved1   = 0;                     // dwReserved1&2 には0をセットする。しないとDSERR_INVALIDPARAMエラーになる
      pDSFXDesc[i].dwReserved2   = 0;
    }
    // pdwResultCodesは無視する。
    hr = st->pDSBuffer8->lpVtbl->SetFX(st->pDSBuffer8, count, pDSFXDesc, NULL);
    free(pDSFXDesc);
  }
  else hr = st->pDSBuffer8->lpVtbl->SetFX(st->pDSBuffer8,  0,      NULL, NULL);

  // 再生中に呼び出したときに出るエラー
  if (hr == DSERR_INVALIDCALL) rb_raise(eSoundBufferError, "SetFX error(DSERR_INVALIDCALL)");
  if (FAILED(hr)) to_raise_an_exception(hr);
  st->effect_list = rb_obj_freeze(vargs);
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXGargle(VALUE self, VALUE nth)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXGargle  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_GARGLE,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXGargle8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXGargle8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXGargle8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(2,  UINT2NUM(dsfx.dwRateHz),
                                  UINT2NUM(dsfx.dwWaveShape));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXGargle(int argc, VALUE *argv, VALUE self)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXGargle  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 3) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.dwRateHz    = NUM2UINT(argv[1]);
  dsfx.dwWaveShape = NUM2UINT(argv[2]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_GARGLE,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXGargle8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXGargle8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXGargle8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXChorus(VALUE self, VALUE nth)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXChorus  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_CHORUS,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXChorus8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXChorus8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXChorus8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(7,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fDepth),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fFrequency),
                                  INT2NUM(dsfx.lWaveform),
                                  rb_float_new(dsfx.fDelay),
                                  INT2NUM(dsfx.lPhase));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXChorus(int argc, VALUE *argv, VALUE self)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXChorus  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 8) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix = (float)NUM2DBL(argv[1]);
  dsfx.fDepth     = (float)NUM2DBL(argv[2]);
  dsfx.fFeedback  = (float)NUM2DBL(argv[3]);
  dsfx.fFrequency = (float)NUM2DBL(argv[4]);
  dsfx.lWaveform  = NUM2INT(argv[5]);
  dsfx.fDelay     = (float)NUM2DBL(argv[6]);
  dsfx.lPhase     = NUM2INT(argv[7]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_CHORUS,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXChorus8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXChorus8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXChorus8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXFlanger(VALUE self, VALUE nth)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXFlanger  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_FLANGER,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXFlanger8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXFlanger8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXFlanger8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(7,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fDepth),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fFrequency),
                                  INT2NUM(dsfx.lWaveform),
                                  rb_float_new(dsfx.fDelay),
                                  INT2NUM(dsfx.lPhase));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXFlanger(int argc, VALUE *argv, VALUE self)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXFlanger  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 8) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix = (float)NUM2DBL(argv[1]);
  dsfx.fDepth     = (float)NUM2DBL(argv[2]);
  dsfx.fFeedback  = (float)NUM2DBL(argv[3]);
  dsfx.fFrequency = (float)NUM2DBL(argv[4]);
  dsfx.lWaveform  = NUM2INT(argv[5]);
  dsfx.fDelay     = (float)NUM2DBL(argv[6]);
  dsfx.lPhase     = NUM2INT(argv[7]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_FLANGER,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXFlanger8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXFlanger8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXFlanger8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXEcho(VALUE self, VALUE nth)
{
  HRESULT   hr;
  LPVOID    pObject;
  DSFXEcho  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_ECHO,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXEcho8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXEcho8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXEcho8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(5,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fLeftDelay),
                                  rb_float_new(dsfx.fRightDelay),
                                  INT2NUM(dsfx.lPanDelay));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXEcho(int argc, VALUE *argv, VALUE self)
{
  HRESULT   hr;
  LPVOID    pObject;
  DSFXEcho  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 6) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix   = (float)NUM2DBL(argv[1]);
  dsfx.fFeedback    = (float)NUM2DBL(argv[2]);
  dsfx.fLeftDelay   = (float)NUM2DBL(argv[3]);
  dsfx.fRightDelay  = (float)NUM2DBL(argv[4]);
  dsfx.lPanDelay    = NUM2INT(argv[5]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_ECHO,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXEcho8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXEcho8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXEcho8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXDistortion(VALUE self, VALUE nth)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXDistortion  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_DISTORTION,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXDistortion8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXDistortion8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXDistortion8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(5,  rb_float_new(dsfx.fGain),
                                  rb_float_new(dsfx.fEdge),
                                  rb_float_new(dsfx.fPostEQCenterFrequency),
                                  rb_float_new(dsfx.fPostEQBandwidth),
                                  rb_float_new(dsfx.fPreLowpassCutoff));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXDistortion(int argc, VALUE *argv, VALUE self)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXDistortion  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 6) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fGain                  = (float)NUM2DBL(argv[1]);
  dsfx.fEdge                  = (float)NUM2DBL(argv[2]);
  dsfx.fPostEQCenterFrequency = (float)NUM2DBL(argv[3]);
  dsfx.fPostEQBandwidth       = (float)NUM2DBL(argv[4]);
  dsfx.fPreLowpassCutoff      = (float)NUM2DBL(argv[5]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_DISTORTION,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXDistortion8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXDistortion8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXDistortion8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXCompressor(VALUE self, VALUE nth)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXCompressor  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_COMPRESSOR,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXCompressor8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXCompressor8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXCompressor8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(6,  rb_float_new(dsfx.fGain),
                                  rb_float_new(dsfx.fAttack),
                                  rb_float_new(dsfx.fRelease),
                                  rb_float_new(dsfx.fThreshold),
                                  rb_float_new(dsfx.fRatio),
                                  rb_float_new(dsfx.fPredelay));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXCompressor(int argc, VALUE *argv, VALUE self)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXCompressor  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 7) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fGain      = (float)NUM2DBL(argv[1]);
  dsfx.fAttack    = (float)NUM2DBL(argv[2]);
  dsfx.fRelease   = (float)NUM2DBL(argv[3]);
  dsfx.fThreshold = (float)NUM2DBL(argv[4]);
  dsfx.fRatio     = (float)NUM2DBL(argv[5]);
  dsfx.fPredelay  = (float)NUM2DBL(argv[6]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_COMPRESSOR,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXCompressor8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXCompressor8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXCompressor8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXParamEq(VALUE self, VALUE nth)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXParamEq  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_PARAMEQ,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXParamEq8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXParamEq8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXParamEq8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(3,  rb_float_new(dsfx.fCenter),
                                  rb_float_new(dsfx.fBandwidth),
                                  rb_float_new(dsfx.fGain));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXParamEq(int argc, VALUE *argv, VALUE self)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXParamEq  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 4) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fCenter    = (float)NUM2DBL(argv[1]);
  dsfx.fBandwidth = (float)NUM2DBL(argv[2]);
  dsfx.fGain      = (float)NUM2DBL(argv[3]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_PARAMEQ,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXParamEq8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXParamEq8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXParamEq8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXI3DL2Reverb(VALUE self, VALUE nth)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXI3DL2Reverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_I3DL2REVERB,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXI3DL2Reverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXI3DL2Reverb8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXI3DL2Reverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(12, INT2NUM(dsfx.lRoom),
                                  INT2NUM(dsfx.lRoomHF),
                                  rb_float_new(dsfx.flRoomRolloffFactor),
                                  rb_float_new(dsfx.flDecayTime),
                                  rb_float_new(dsfx.flDecayHFRatio),
                                  INT2NUM(dsfx.lReflections),
                                  rb_float_new(dsfx.flReflectionsDelay),
                                  INT2NUM(dsfx.lReverb),
                                  rb_float_new(dsfx.flReverbDelay),
                                  rb_float_new(dsfx.flDiffusion),
                                  rb_float_new(dsfx.flDensity),
                                  rb_float_new(dsfx.flHFReference));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXI3DL2Reverb(int argc, VALUE *argv, VALUE self)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXI3DL2Reverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 13) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.lRoom               = NUM2INT(argv[1]);
  dsfx.lRoomHF             = NUM2INT(argv[2]);
  dsfx.flRoomRolloffFactor = (float)NUM2DBL(argv[3]);
  dsfx.flDecayTime         = (float)NUM2DBL(argv[4]);
  dsfx.flDecayHFRatio      = (float)NUM2DBL(argv[5]);
  dsfx.lReflections        = NUM2INT(argv[6]);
  dsfx.flReflectionsDelay  = (float)NUM2DBL(argv[7]);
  dsfx.lReverb             = NUM2INT(argv[8]);
  dsfx.flReverbDelay       = (float)NUM2DBL(argv[9]);
  dsfx.flDiffusion         = (float)NUM2DBL(argv[10]);
  dsfx.flDensity           = (float)NUM2DBL(argv[11]);
  dsfx.flHFReference       = (float)NUM2DBL(argv[12]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_I3DL2REVERB,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXI3DL2Reverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXI3DL2Reverb8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXI3DL2Reverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXWavesReverb(VALUE self, VALUE nth)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXWavesReverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_WAVES_REVERB,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXWavesReverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXWavesReverb8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXWavesReverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(4,  rb_float_new(dsfx.fInGain),
                                  rb_float_new(dsfx.fReverbMix),
                                  rb_float_new(dsfx.fReverbTime),
                                  rb_float_new(dsfx.fHighFreqRTRatio));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXWavesReverb(int argc, VALUE *argv, VALUE self)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXWavesReverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 5) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fInGain          = (float)NUM2DBL(argv[1]);
  dsfx.fReverbMix       = (float)NUM2DBL(argv[2]);
  dsfx.fReverbTime      = (float)NUM2DBL(argv[3]);
  dsfx.fHighFreqRTRatio = (float)NUM2DBL(argv[4]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_WAVES_REVERB,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXWavesReverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXWavesReverb8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXWavesReverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

//
//
//
static VALUE
SoundBuffer_get_effect_param(VALUE self, VALUE nth)
{
  struct SoundBuffer *st = get_st(self);

  if (!st->ctrlfx_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  switch (NUM2UINT(rb_ary_entry(st->effect_list, NUM2UINT(nth)))) {
    case FX_GARGLE:
      return SoundBuffer_GetAllParameters_DSFXGargle(self, nth);
      break;
    case FX_CHORUS:
      return SoundBuffer_GetAllParameters_DSFXChorus(self, nth);
      break;
    case FX_FLANGER:
      return SoundBuffer_GetAllParameters_DSFXFlanger(self, nth);
      break;
    case FX_ECHO:
      return SoundBuffer_GetAllParameters_DSFXEcho(self, nth);
      break;
    case FX_DISTORTION:
      return SoundBuffer_GetAllParameters_DSFXDistortion(self, nth);
      break;
    case FX_COMPRESSOR:
      return SoundBuffer_GetAllParameters_DSFXCompressor(self, nth);
      break;
    case FX_PARAM_EQ:
      return SoundBuffer_GetAllParameters_DSFXParamEq(self, nth);
      break;
    case FX_I3DL2_REVERB:
      return SoundBuffer_GetAllParameters_DSFXI3DL2Reverb(self, nth);
      break;
    case FX_WAVES_REVERB:
      return SoundBuffer_GetAllParameters_DSFXWavesReverb(self, nth);
      break;
    default:
      rb_raise(rb_eTypeError, "not valid value");
      break;
  }
}
//
//
//
static VALUE
SoundBuffer_set_effect_param(int argc, VALUE *argv, VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  if (!st->ctrlfx_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  switch (NUM2UINT(rb_ary_entry(st->effect_list, NUM2UINT(argv[0])))) {
    case FX_GARGLE:
      return SoundBuffer_SetAllParameters_DSFXGargle(argc, argv, self);
      break;
    case FX_CHORUS:
      return SoundBuffer_SetAllParameters_DSFXChorus(argc, argv, self);
      break;
    case FX_FLANGER:
      return SoundBuffer_SetAllParameters_DSFXFlanger(argc, argv, self);
      break;
    case FX_ECHO:
      return SoundBuffer_SetAllParameters_DSFXEcho(argc, argv, self);
      break;
    case FX_DISTORTION:
      return SoundBuffer_SetAllParameters_DSFXDistortion(argc, argv, self);
      break;
    case FX_COMPRESSOR:
      return SoundBuffer_SetAllParameters_DSFXCompressor(argc, argv, self);
      break;
    case FX_PARAM_EQ:
      return SoundBuffer_SetAllParameters_DSFXParamEq(argc, argv, self);
      break;
    case FX_I3DL2_REVERB:
      return SoundBuffer_SetAllParameters_DSFXI3DL2Reverb(argc, argv, self);
      break;
    case FX_WAVES_REVERB:
      return SoundBuffer_SetAllParameters_DSFXWavesReverb(argc, argv, self);
      break;
    default:
      rb_raise(rb_eTypeError, "not valid value");
      break;
  }
}

/*
 *
 */
static VALUE
SoundBuffer_stop_and_play(VALUE self, VALUE vblock)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
  if (FAILED(hr)) to_raise_an_exception(hr);

  if (rb_block_given_p()) rb_yield(self);

  hr = st->pDSBuffer8->lpVtbl->Play(st->pDSBuffer8, 0, 0, st->repeat_flag ? DSBPLAY_LOOPING : 0);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return self;
}

// Rubyのクラス定義
void
Init_SoundBuffer(void)
{
  eSoundBufferError = rb_define_class("SoundBufferError", rb_eRuntimeError);

  cSoundBuffer = rb_define_class("SoundBuffer", rb_cObject);

  rb_define_alloc_func(cSoundBuffer, SoundBuffer_allocate);
  rb_define_private_method(cSoundBuffer, "initialize", SoundBuffer_initialize, -1);

  rb_define_method(cSoundBuffer, "duplicate",         SoundBuffer_duplicate,         0);

  rb_define_method(cSoundBuffer, "dispose",           SoundBuffer_dispose,           0);
  rb_define_method(cSoundBuffer, "disposed?",         SoundBuffer_disposed,          0);
  rb_define_method(cSoundBuffer, "flash",             SoundBuffer_flash,             0);
  rb_define_method(cSoundBuffer, "pause",             SoundBuffer_pause,             0);
  rb_define_method(cSoundBuffer, "pausing?",          SoundBuffer_pausing,           0);
  rb_define_method(cSoundBuffer, "play",              SoundBuffer_play,              0);
  rb_define_method(cSoundBuffer, "play_pos=",         SoundBuffer_play_pos_eql,      1);
  rb_define_method(cSoundBuffer, "play_pos",          SoundBuffer_play_pos,          0);
  rb_define_method(cSoundBuffer, "playing?",          SoundBuffer_playing,           0);
  rb_define_method(cSoundBuffer, "repeat",            SoundBuffer_repeat,            0);
  rb_define_method(cSoundBuffer, "repeating?",        SoundBuffer_repeating,         0);
  rb_define_method(cSoundBuffer, "size",              SoundBuffer_size,              0);
  rb_define_method(cSoundBuffer, "stop",              SoundBuffer_stop,              0);
  rb_define_method(cSoundBuffer, "stop_and_play",     SoundBuffer_stop_and_play,     0);
  rb_define_method(cSoundBuffer, "to_s",              SoundBuffer_to_s,              0);
  rb_define_method(cSoundBuffer, "write",             SoundBuffer_write,            -1);
  rb_define_method(cSoundBuffer, "write_sync",        SoundBuffer_write_sync,        1);
  rb_define_method(cSoundBuffer, "effectable?",       SoundBuffer_get_effectable,    0);

  rb_define_method(cSoundBuffer, "channels",          SoundBuffer_get_channels,          0);
  rb_define_method(cSoundBuffer, "samples_per_sec",   SoundBuffer_get_samples_per_sec,   0);
  rb_define_method(cSoundBuffer, "bits_per_sample",   SoundBuffer_get_bits_per_sample,   0);
  rb_define_method(cSoundBuffer, "block_align",       SoundBuffer_get_block_align,       0);
  rb_define_method(cSoundBuffer, "avg_bytes_per_sec", SoundBuffer_get_avg_bytes_per_sec, 0);

  rb_define_method(cSoundBuffer, "get_volume",        SoundBuffer_get_volume,        0);
  rb_define_method(cSoundBuffer, "set_volume",        SoundBuffer_set_volume,        1);
  rb_define_method(cSoundBuffer, "get_pan",           SoundBuffer_get_pan,           0);
  rb_define_method(cSoundBuffer, "set_pan",           SoundBuffer_set_pan,           1);
  rb_define_method(cSoundBuffer, "get_frequency",     SoundBuffer_get_frequency,     0);
  rb_define_method(cSoundBuffer, "set_frequency",     SoundBuffer_set_frequency,     1);
  rb_define_method(cSoundBuffer, "get_effect",        SoundBuffer_get_effect,        0);
  rb_define_method(cSoundBuffer, "set_effect",        SoundBuffer_set_effect,       -2);
  rb_define_method(cSoundBuffer, "get_effect_param",  SoundBuffer_get_effect_param,  1);
  rb_define_method(cSoundBuffer, "set_effect_param",  SoundBuffer_set_effect_param, -1);

  rb_define_alias(cSoundBuffer, "volume",     "get_volume");
  rb_define_alias(cSoundBuffer, "volume=",    "set_volume");
  rb_define_alias(cSoundBuffer, "pan",        "get_pan");
  rb_define_alias(cSoundBuffer, "pan=",       "set_pan");
  rb_define_alias(cSoundBuffer, "frequency",  "get_frequency");
  rb_define_alias(cSoundBuffer, "frequency=", "set_frequency");
  rb_define_alias(cSoundBuffer, "pos",        "play_pos");
  rb_define_alias(cSoundBuffer, "pos=",       "play_pos=");

  /*
   * Consts
   */
  rb_define_const(cSoundBuffer, "FX_GARGLE",       INT2NUM(FX_GARGLE));
  rb_define_const(cSoundBuffer, "FX_CHORUS",       INT2NUM(FX_CHORUS));
  rb_define_const(cSoundBuffer, "FX_FLANGER",      INT2NUM(FX_FLANGER));
  rb_define_const(cSoundBuffer, "FX_ECHO",         INT2NUM(FX_ECHO));
  rb_define_const(cSoundBuffer, "FX_DISTORTION",   INT2NUM(FX_DISTORTION));
  rb_define_const(cSoundBuffer, "FX_COMPRESSOR",   INT2NUM(FX_COMPRESSOR));
  rb_define_const(cSoundBuffer, "FX_PARAM_EQ",     INT2NUM(FX_PARAM_EQ));
  rb_define_const(cSoundBuffer, "FX_I3DL2_REVERB", INT2NUM(FX_I3DL2_REVERB));
  rb_define_const(cSoundBuffer, "FX_WAVES_REVERB", INT2NUM(FX_WAVES_REVERB));
  /*
   * IDirectSoundFXGargle
   */
  rb_define_const(cSoundBuffer, "DSFXGARGLE_WAVE_TRIANGLE",                   INT2NUM(DSFXGARGLE_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_WAVE_SQUARE",                     INT2NUM(DSFXGARGLE_WAVE_SQUARE));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_RATEHZ_MIN",                      INT2NUM(DSFXGARGLE_RATEHZ_MIN));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_RATEHZ_MAX",                      INT2NUM(DSFXGARGLE_RATEHZ_MAX));
  /*
   * IDirectSoundFXChorus
   */
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WAVE_TRIANGLE",                   INT2NUM(DSFXCHORUS_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WAVE_SIN",                        INT2NUM(DSFXCHORUS_WAVE_SIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WETDRYMIX_MIN",                   rb_float_new(DSFXCHORUS_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WETDRYMIX_MAX",                   rb_float_new(DSFXCHORUS_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DEPTH_MIN",                       rb_float_new(DSFXCHORUS_DEPTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DEPTH_MAX",                       rb_float_new(DSFXCHORUS_DEPTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FEEDBACK_MIN",                    rb_float_new(DSFXCHORUS_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FEEDBACK_MAX",                    rb_float_new(DSFXCHORUS_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FREQUENCY_MIN",                   rb_float_new(DSFXCHORUS_FREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FREQUENCY_MAX",                   rb_float_new(DSFXCHORUS_FREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DELAY_MIN",                       rb_float_new(DSFXCHORUS_DELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DELAY_MAX",                       rb_float_new(DSFXCHORUS_DELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_MIN",                       INT2NUM(DSFXCHORUS_PHASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_MAX",                       INT2NUM(DSFXCHORUS_PHASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_NEG_180",                   INT2NUM(DSFXCHORUS_PHASE_NEG_180));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_NEG_90",                    INT2NUM(DSFXCHORUS_PHASE_NEG_90));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_ZERO",                      INT2NUM(DSFXCHORUS_PHASE_ZERO));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_90",                        INT2NUM(DSFXCHORUS_PHASE_90));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_180",                       INT2NUM(DSFXCHORUS_PHASE_180));
  /*
   * IDirectSoundFXFlanger
   */
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WAVE_TRIANGLE",                  INT2NUM(DSFXFLANGER_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WAVE_SIN",                       INT2NUM(DSFXFLANGER_WAVE_SIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WETDRYMIX_MIN",                  rb_float_new(DSFXFLANGER_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WETDRYMIX_MAX",                  rb_float_new(DSFXFLANGER_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FREQUENCY_MIN",                  rb_float_new(DSFXFLANGER_FREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FREQUENCY_MAX",                  rb_float_new(DSFXFLANGER_FREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DEPTH_MIN",                      rb_float_new(DSFXFLANGER_DEPTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DEPTH_MAX",                      rb_float_new(DSFXFLANGER_DEPTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_MIN",                      INT2NUM(DSFXFLANGER_PHASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_MAX",                      INT2NUM(DSFXFLANGER_PHASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FEEDBACK_MIN",                   rb_float_new(DSFXFLANGER_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FEEDBACK_MAX",                   rb_float_new(DSFXFLANGER_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DELAY_MIN",                      rb_float_new(DSFXFLANGER_DELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DELAY_MAX",                      rb_float_new(DSFXFLANGER_DELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_NEG_180",                  INT2NUM(DSFXFLANGER_PHASE_NEG_180));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_NEG_90",                   INT2NUM(DSFXFLANGER_PHASE_NEG_90));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_ZERO",                     INT2NUM(DSFXFLANGER_PHASE_ZERO));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_90",                       INT2NUM(DSFXFLANGER_PHASE_90));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_180",                      INT2NUM(DSFXFLANGER_PHASE_180));
  /*
   * IDirectSoundFXEcho
   */
  rb_define_const(cSoundBuffer, "DSFXECHO_WETDRYMIX_MIN",                     rb_float_new(DSFXECHO_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_WETDRYMIX_MAX",                     rb_float_new(DSFXECHO_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_FEEDBACK_MIN",                      rb_float_new(DSFXECHO_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_FEEDBACK_MAX",                      rb_float_new(DSFXECHO_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_LEFTDELAY_MIN",                     rb_float_new(DSFXECHO_LEFTDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_LEFTDELAY_MAX",                     rb_float_new(DSFXECHO_LEFTDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_RIGHTDELAY_MIN",                    rb_float_new(DSFXECHO_RIGHTDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_RIGHTDELAY_MAX",                    rb_float_new(DSFXECHO_RIGHTDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_PANDELAY_MIN",                      INT2NUM(DSFXECHO_PANDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_PANDELAY_MAX",                      INT2NUM(DSFXECHO_PANDELAY_MAX));
  /*
   * IDirectSoundFXDistortion
   */
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_GAIN_MIN",                    rb_float_new(DSFXDISTORTION_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_GAIN_MAX",                    rb_float_new(DSFXDISTORTION_GAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_EDGE_MIN",                    rb_float_new(DSFXDISTORTION_EDGE_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_EDGE_MAX",                    rb_float_new(DSFXDISTORTION_EDGE_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQCENTERFREQUENCY_MIN",   rb_float_new(DSFXDISTORTION_POSTEQCENTERFREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQCENTERFREQUENCY_MAX",   rb_float_new(DSFXDISTORTION_POSTEQCENTERFREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQBANDWIDTH_MIN",         rb_float_new(DSFXDISTORTION_POSTEQBANDWIDTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQBANDWIDTH_MAX",         rb_float_new(DSFXDISTORTION_POSTEQBANDWIDTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_PRELOWPASSCUTOFF_MIN",        rb_float_new(DSFXDISTORTION_PRELOWPASSCUTOFF_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_PRELOWPASSCUTOFF_MAX",        rb_float_new(DSFXDISTORTION_PRELOWPASSCUTOFF_MAX));
  /*
   * IDirectSoundFXCompressor
   */
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_GAIN_MIN",                    rb_float_new(DSFXCOMPRESSOR_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_GAIN_MAX",                    rb_float_new(DSFXCOMPRESSOR_GAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_ATTACK_MIN",                  rb_float_new(DSFXCOMPRESSOR_ATTACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_ATTACK_MAX",                  rb_float_new(DSFXCOMPRESSOR_ATTACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RELEASE_MIN",                 rb_float_new(DSFXCOMPRESSOR_RELEASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RELEASE_MAX",                 rb_float_new(DSFXCOMPRESSOR_RELEASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_THRESHOLD_MIN",               rb_float_new(DSFXCOMPRESSOR_THRESHOLD_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_THRESHOLD_MAX",               rb_float_new(DSFXCOMPRESSOR_THRESHOLD_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RATIO_MIN",                   rb_float_new(DSFXCOMPRESSOR_RATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RATIO_MAX",                   rb_float_new(DSFXCOMPRESSOR_RATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_PREDELAY_MIN",                rb_float_new(DSFXCOMPRESSOR_PREDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_PREDELAY_MAX",                rb_float_new(DSFXCOMPRESSOR_PREDELAY_MAX));
  /*
   * IDirectSoundFXParamEq
   */
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_CENTER_MIN",                     rb_float_new(DSFXPARAMEQ_CENTER_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_CENTER_MAX",                     rb_float_new(DSFXPARAMEQ_CENTER_MAX));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_BANDWIDTH_MIN",                  rb_float_new(DSFXPARAMEQ_BANDWIDTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_BANDWIDTH_MAX",                  rb_float_new(DSFXPARAMEQ_BANDWIDTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_GAIN_MIN",                       rb_float_new(DSFXPARAMEQ_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_GAIN_MAX",                       rb_float_new(DSFXPARAMEQ_GAIN_MAX));
  /*
   * IDirectSoundFXI3DL2Reverb
   */
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_MIN",                  INT2NUM(DSFX_I3DL2REVERB_ROOM_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_MAX",                  INT2NUM(DSFX_I3DL2REVERB_ROOM_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_DEFAULT",              INT2NUM(DSFX_I3DL2REVERB_ROOM_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_MIN",                INT2NUM(DSFX_I3DL2REVERB_ROOMHF_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_MAX",                INT2NUM(DSFX_I3DL2REVERB_ROOMHF_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_DEFAULT",            INT2NUM(DSFX_I3DL2REVERB_ROOMHF_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN",     rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX",     rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_DEFAULT", rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_MIN",             rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_MAX",             rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_DEFAULT",         rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_MIN",          rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_MAX",          rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_DEFAULT",      rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_MIN",           INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_MAX",           INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_DEFAULT",       INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN",      rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_MAX",      rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_DEFAULT",  rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_MIN",                INT2NUM(DSFX_I3DL2REVERB_REVERB_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_MAX",                INT2NUM(DSFX_I3DL2REVERB_REVERB_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_DEFAULT",            INT2NUM(DSFX_I3DL2REVERB_REVERB_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_MIN",           rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_MAX",           rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_DEFAULT",       rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_MIN",             rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_MAX",             rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_DEFAULT",         rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_MIN",               rb_float_new(DSFX_I3DL2REVERB_DENSITY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_MAX",               rb_float_new(DSFX_I3DL2REVERB_DENSITY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_DEFAULT",           rb_float_new(DSFX_I3DL2REVERB_DENSITY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_MIN",           rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_MAX",           rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_DEFAULT",       rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_MIN",               INT2NUM(DSFX_I3DL2REVERB_QUALITY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_MAX",               INT2NUM(DSFX_I3DL2REVERB_QUALITY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_DEFAULT",           INT2NUM(DSFX_I3DL2REVERB_QUALITY_DEFAULT));
  /*
   * IDirectSoundFXWavesReverb
   */
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_MIN",                rb_float_new(DSFX_WAVESREVERB_INGAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_MAX",                rb_float_new(DSFX_WAVESREVERB_INGAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_DEFAULT",            rb_float_new(DSFX_WAVESREVERB_INGAIN_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_MIN",             rb_float_new(DSFX_WAVESREVERB_REVERBMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_MAX",             rb_float_new(DSFX_WAVESREVERB_REVERBMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_DEFAULT",         rb_float_new(DSFX_WAVESREVERB_REVERBMIX_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_MIN",            rb_float_new(DSFX_WAVESREVERB_REVERBTIME_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_MAX",            rb_float_new(DSFX_WAVESREVERB_REVERBTIME_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_DEFAULT",        rb_float_new(DSFX_WAVESREVERB_REVERBTIME_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_MIN",       rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_MAX",       rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_DEFAULT",   rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_DEFAULT));
  /*
   * Flags
   */
  rb_define_const(cSoundBuffer, "DSCAPS_PRIMARYMONO",                         INT2NUM(DSCAPS_PRIMARYMONO));
  rb_define_const(cSoundBuffer, "DSCAPS_PRIMARYSTEREO",                       INT2NUM(DSCAPS_PRIMARYSTEREO));
  rb_define_const(cSoundBuffer, "DSCAPS_PRIMARY8BIT",                         INT2NUM(DSCAPS_PRIMARY8BIT));
  rb_define_const(cSoundBuffer, "DSCAPS_PRIMARY16BIT",                        INT2NUM(DSCAPS_PRIMARY16BIT));
  rb_define_const(cSoundBuffer, "DSCAPS_CONTINUOUSRATE",                      INT2NUM(DSCAPS_CONTINUOUSRATE));
  rb_define_const(cSoundBuffer, "DSCAPS_EMULDRIVER",                          INT2NUM(DSCAPS_EMULDRIVER));
  rb_define_const(cSoundBuffer, "DSCAPS_CERTIFIED",                           INT2NUM(DSCAPS_CERTIFIED));
  rb_define_const(cSoundBuffer, "DSCAPS_SECONDARYMONO",                       INT2NUM(DSCAPS_SECONDARYMONO));
  rb_define_const(cSoundBuffer, "DSCAPS_SECONDARYSTEREO",                     INT2NUM(DSCAPS_SECONDARYSTEREO));
  rb_define_const(cSoundBuffer, "DSCAPS_SECONDARY8BIT",                       INT2NUM(DSCAPS_SECONDARY8BIT));
  rb_define_const(cSoundBuffer, "DSCAPS_SECONDARY16BIT",                      INT2NUM(DSCAPS_SECONDARY16BIT));
  rb_define_const(cSoundBuffer, "DSSCL_NORMAL",                               INT2NUM(DSSCL_NORMAL));
  rb_define_const(cSoundBuffer, "DSSCL_PRIORITY",                             INT2NUM(DSSCL_PRIORITY));
  rb_define_const(cSoundBuffer, "DSSCL_EXCLUSIVE",                            INT2NUM(DSSCL_EXCLUSIVE));
  rb_define_const(cSoundBuffer, "DSSCL_WRITEPRIMARY",                         INT2NUM(DSSCL_WRITEPRIMARY));
  rb_define_const(cSoundBuffer, "DSSPEAKER_DIRECTOUT",                        INT2NUM(DSSPEAKER_DIRECTOUT));
  rb_define_const(cSoundBuffer, "DSSPEAKER_HEADPHONE",                        INT2NUM(DSSPEAKER_HEADPHONE));
  rb_define_const(cSoundBuffer, "DSSPEAKER_MONO",                             INT2NUM(DSSPEAKER_MONO));
  rb_define_const(cSoundBuffer, "DSSPEAKER_QUAD",                             INT2NUM(DSSPEAKER_QUAD));
  rb_define_const(cSoundBuffer, "DSSPEAKER_STEREO",                           INT2NUM(DSSPEAKER_STEREO));
  rb_define_const(cSoundBuffer, "DSSPEAKER_SURROUND",                         INT2NUM(DSSPEAKER_SURROUND));
  rb_define_const(cSoundBuffer, "DSSPEAKER_5POINT1",                          INT2NUM(DSSPEAKER_5POINT1));
  rb_define_const(cSoundBuffer, "DSSPEAKER_7POINT1",                          INT2NUM(DSSPEAKER_7POINT1));
  rb_define_const(cSoundBuffer, "DSSPEAKER_7POINT1_SURROUND",                 INT2NUM(DSSPEAKER_7POINT1_SURROUND));
  rb_define_const(cSoundBuffer, "DSSPEAKER_5POINT1_SURROUND",                 INT2NUM(DSSPEAKER_5POINT1_SURROUND));
  rb_define_const(cSoundBuffer, "DSSPEAKER_GEOMETRY_MIN",                     INT2NUM(DSSPEAKER_GEOMETRY_MIN));
  rb_define_const(cSoundBuffer, "DSSPEAKER_GEOMETRY_NARROW",                  INT2NUM(DSSPEAKER_GEOMETRY_NARROW));
  rb_define_const(cSoundBuffer, "DSSPEAKER_GEOMETRY_WIDE",                    INT2NUM(DSSPEAKER_GEOMETRY_WIDE));
  rb_define_const(cSoundBuffer, "DSSPEAKER_GEOMETRY_MAX",                     INT2NUM(DSSPEAKER_GEOMETRY_MAX));
  rb_define_const(cSoundBuffer, "DSBCAPS_PRIMARYBUFFER",                      INT2NUM(DSBCAPS_PRIMARYBUFFER));
  rb_define_const(cSoundBuffer, "DSBCAPS_STATIC",                             INT2NUM(DSBCAPS_STATIC));
  rb_define_const(cSoundBuffer, "DSBCAPS_LOCHARDWARE",                        INT2NUM(DSBCAPS_LOCHARDWARE));
  rb_define_const(cSoundBuffer, "DSBCAPS_LOCSOFTWARE",                        INT2NUM(DSBCAPS_LOCSOFTWARE));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRL3D",                             INT2NUM(DSBCAPS_CTRL3D));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRLFREQUENCY",                      INT2NUM(DSBCAPS_CTRLFREQUENCY));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRLPAN",                            INT2NUM(DSBCAPS_CTRLPAN));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRLVOLUME",                         INT2NUM(DSBCAPS_CTRLVOLUME));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRLPOSITIONNOTIFY",                 INT2NUM(DSBCAPS_CTRLPOSITIONNOTIFY));
  rb_define_const(cSoundBuffer, "DSBCAPS_CTRLFX",                             INT2NUM(DSBCAPS_CTRLFX));
  rb_define_const(cSoundBuffer, "DSBCAPS_STICKYFOCUS",                        INT2NUM(DSBCAPS_STICKYFOCUS));
  rb_define_const(cSoundBuffer, "DSBCAPS_GLOBALFOCUS",                        INT2NUM(DSBCAPS_GLOBALFOCUS));
  rb_define_const(cSoundBuffer, "DSBCAPS_GETCURRENTPOSITION2",                INT2NUM(DSBCAPS_GETCURRENTPOSITION2));
  rb_define_const(cSoundBuffer, "DSBCAPS_MUTE3DATMAXDISTANCE",                INT2NUM(DSBCAPS_MUTE3DATMAXDISTANCE));
  rb_define_const(cSoundBuffer, "DSBCAPS_LOCDEFER",                           INT2NUM(DSBCAPS_LOCDEFER));
  rb_define_const(cSoundBuffer, "DSBCAPS_TRUEPLAYPOSITION",                   INT2NUM(DSBCAPS_TRUEPLAYPOSITION));
  rb_define_const(cSoundBuffer, "DSBPLAY_LOOPING",                            INT2NUM(DSBPLAY_LOOPING));
  rb_define_const(cSoundBuffer, "DSBPLAY_LOCHARDWARE",                        INT2NUM(DSBPLAY_LOCHARDWARE));
  rb_define_const(cSoundBuffer, "DSBPLAY_LOCSOFTWARE",                        INT2NUM(DSBPLAY_LOCSOFTWARE));
  rb_define_const(cSoundBuffer, "DSBPLAY_TERMINATEBY_TIME",                   INT2NUM(DSBPLAY_TERMINATEBY_TIME));
  rb_define_const(cSoundBuffer, "DSBPLAY_TERMINATEBY_DISTANCE",               INT2NUM(DSBPLAY_TERMINATEBY_DISTANCE));
  rb_define_const(cSoundBuffer, "DSBPLAY_TERMINATEBY_PRIORITY",               INT2NUM(DSBPLAY_TERMINATEBY_PRIORITY));
  rb_define_const(cSoundBuffer, "DSBSTATUS_PLAYING",                          INT2NUM(DSBSTATUS_PLAYING));
  rb_define_const(cSoundBuffer, "DSBSTATUS_BUFFERLOST",                       INT2NUM(DSBSTATUS_BUFFERLOST));
  rb_define_const(cSoundBuffer, "DSBSTATUS_LOOPING",                          INT2NUM(DSBSTATUS_LOOPING));
  rb_define_const(cSoundBuffer, "DSBSTATUS_LOCHARDWARE",                      INT2NUM(DSBSTATUS_LOCHARDWARE));
  rb_define_const(cSoundBuffer, "DSBSTATUS_LOCSOFTWARE",                      INT2NUM(DSBSTATUS_LOCSOFTWARE));
  rb_define_const(cSoundBuffer, "DSBSTATUS_TERMINATED",                       INT2NUM(DSBSTATUS_TERMINATED));
  rb_define_const(cSoundBuffer, "DSBLOCK_FROMWRITECURSOR",                    INT2NUM(DSBLOCK_FROMWRITECURSOR));
  rb_define_const(cSoundBuffer, "DSBLOCK_ENTIREBUFFER",                       INT2NUM(DSBLOCK_ENTIREBUFFER));
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_ORIGINAL",                      INT2NUM(DSBFREQUENCY_ORIGINAL));
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_MIN",                           INT2NUM(DSBFREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_MAX",                           INT2NUM(DSBFREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSBPAN_LEFT",                                INT2NUM(DSBPAN_LEFT));
  rb_define_const(cSoundBuffer, "DSBPAN_CENTER",                              INT2NUM(DSBPAN_CENTER));
  rb_define_const(cSoundBuffer, "DSBPAN_RIGHT",                               INT2NUM(DSBPAN_RIGHT));
  rb_define_const(cSoundBuffer, "DSBVOLUME_MIN",                              INT2NUM(DSBVOLUME_MIN));
  rb_define_const(cSoundBuffer, "DSBVOLUME_MAX",                              INT2NUM(DSBVOLUME_MAX));
  rb_define_const(cSoundBuffer, "DSBSIZE_MIN",                                INT2NUM(DSBSIZE_MIN));
  rb_define_const(cSoundBuffer, "DSBSIZE_MAX",                                INT2NUM(DSBSIZE_MAX));
  rb_define_const(cSoundBuffer, "DSBSIZE_FX_MIN",                             INT2NUM(DSBSIZE_FX_MIN));
  rb_define_const(cSoundBuffer, "DSBNOTIFICATIONS_MAX",                       INT2NUM(DSBNOTIFICATIONS_MAX));
}

// 終了時に実行されるENDブロックに登録する関数
static void SoundBuffer_shutdown(VALUE obj)
{
  // shutduwn+すべてのSoundTestが解放されたらDirectSound解放
  g_refcount--;
  if (g_refcount == 0) {
    g_pDSound->lpVtbl->Release(g_pDSound);
    CoUninitialize();
  }
}

void Init_soundbuffer(void)
{
  HWND       hWnd;
  HINSTANCE  hInstance;
  WNDCLASSEX wcex;
  HRESULT    hr;

  // COM初期化
  CoInitialize(NULL);

  // ウィンドウクラス設定
  hInstance = (HINSTANCE)GetModuleHandle(NULL);
  wcex.cbSize        = sizeof(WNDCLASSEX);
  wcex.style         = 0;
  wcex.lpfnWndProc   = DefWindowProc;
  wcex.cbClsExtra    = 0;
  wcex.cbWndExtra    = 0;
  wcex.hInstance     = hInstance;
  wcex.hIcon         = 0;
  wcex.hIconSm       = 0;
  wcex.hCursor       = 0;
  wcex.hbrBackground = 0;
  wcex.lpszMenuName  = NULL;
  wcex.lpszClassName = "SoundBuffer";

  // ウィンドウ生成
  RegisterClassEx(&wcex);
  hWnd = CreateWindow("SoundBuffer", "", 0, 0, 0, 0, 0, 0, NULL, hInstance, NULL);

  // DirectSoundオブジェクト生成
  hr = DirectSoundCreate8(&DSDEVID_DefaultPlayback, &g_pDSound, NULL);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "DirectSoundCreate8 error");

  // 協調レベル設定
  hr = g_pDSound->lpVtbl->SetCooperativeLevel(g_pDSound, hWnd, DSSCL_PRIORITY);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetCooperativeLevel error");

  g_refcount++;

  // 終了時に実行する関数
  rb_set_end_proc(SoundBuffer_shutdown, Qnil);

  // SoundTestクラス生成
  Init_SoundBuffer();
}
