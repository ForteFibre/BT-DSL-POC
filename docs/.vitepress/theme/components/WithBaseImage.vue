<script setup lang="ts">
import { withBase } from 'vitepress';

type Props = {
  /**
   * Path under VitePress `base`.
   * Prefer absolute paths like `/railroad/program.svg`.
   */
  src: string;
};

defineProps<Props>();
</script>

<template>
  <img :src="withBase(src)" v-bind="$attrs" />
</template>
