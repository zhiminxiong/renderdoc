import rdtest

class D3D11_Groupshared(rdtest.Groupshared):
    internal = False
    demos_test_name = 'D3D11_Groupshared'

    def check_support(self, **kwargs):
        # Only allow this if explicitly run
        if kwargs['test_include'] == self.demos_test_name:
            return True, ''
        return False, 'Disabled test'
