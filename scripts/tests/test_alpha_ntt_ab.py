import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from benchmark_alpha_ntt_ab import validate_h3_result
from summarize_cloudlab_profiles import ProfileError


class H3ProfileTests(unittest.TestCase):
    def setUp(self):
        sample = {'server_total_ms':10.,'client_query_ms':1.,'client_decode_ms':1.,
                  'response_serialize_ms':1.,'stages_ms':{'prepare':2.,'scan':8.}}
        self.data = {'leaf_count':4096,'entry_bits':256,'measured_trials':2,'warmup_count':3,
                     'candidates':[{'eligible':True,'shape':{'bundle_arity':3},
                                    'samples':[sample,copy.deepcopy(sample)],
                                    'target_noise_result':{'decrypts_correctly':True,
                                                          'observed_key_coverage_matches_tag':True}}]}

    def validate(self):
        return validate_h3_result(self.data, pathlib.Path('profile.json'), 2, 4096)

    def test_h3_only_result_does_not_require_an_h1_control(self):
        self.assertIs(self.validate(), self.data['candidates'][0])

    def test_wrong_decryption_is_rejected(self):
        self.data['candidates'][0]['target_noise_result']['decrypts_correctly'] = False
        with self.assertRaisesRegex(ValueError, 'decryption'):
            self.validate()

    def test_incomplete_measurements_are_rejected(self):
        self.data['candidates'][0]['samples'].pop()
        with self.assertRaises(ProfileError):
            self.validate()

    def test_h1_cannot_be_reported_as_h3(self):
        self.data['candidates'][0]['shape']['bundle_arity'] = 1
        with self.assertRaisesRegex(ValueError, 'h3'):
            self.validate()


if __name__ == '__main__':
    unittest.main()
