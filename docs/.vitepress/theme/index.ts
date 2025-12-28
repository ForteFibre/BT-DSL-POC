import DefaultTheme from 'vitepress/theme';
import './custom.css';
import WithBaseImage from './components/WithBaseImage.vue';

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) {
    app.component('WithBaseImage', WithBaseImage);
  },
};
